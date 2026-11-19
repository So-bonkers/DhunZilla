#include "engine/wav.hpp"

#include <cmath>
#include <cstring>

namespace op::wav {
namespace {

void put_u32_le(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}
void put_u16_le(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
}

uint32_t rd_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rd_u16_le(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

}  // namespace

int16_t float_to_pcm16(float x) {
    if (!(x > -1.0f)) {  // handles NaN and <= -1.0 → floor; NaN maps to 0 (documented choice)
        if (std::isnan(x)) return 0;
        if (x <= -1.0f) return -32768;
    }
    if (x >= 1.0f) return 32767;
    // Round half to even.
    const float scaled = x * 32768.0f;
    const float floorv = std::floor(scaled);
    const float r = scaled - floorv;
    int64_t q;
    if (r > 0.5f) q = static_cast<int64_t>(floorv) + 1;
    else if (r < 0.5f) q = static_cast<int64_t>(floorv);
    else {
        const int64_t f = static_cast<int64_t>(floorv);
        q = (f & 1) ? f + 1 : f;  // exact .5 → nearest even integer
    }
    if (q > 32767) q = 32767;
    if (q < -32768) q = -32768;
    return static_cast<int16_t>(q);
}

int32_t float_to_pcm24(float x) {
    // Mirrors float_to_pcm16 exactly with scale 8388608 (spec render-harness.md, WP-0149).
    if (!(x > -1.0f)) {  // handles NaN and <= -1.0 → floor; NaN maps to 0 (documented choice)
        if (std::isnan(x)) return 0;
        if (x <= -1.0f) return -8388608;
    }
    if (x >= 1.0f) return 8388607;
    // Round half to even.
    const float scaled = x * 8388608.0f;
    const float floorv = std::floor(scaled);
    const float r = scaled - floorv;
    int64_t q;
    if (r > 0.5f) q = static_cast<int64_t>(floorv) + 1;
    else if (r < 0.5f) q = static_cast<int64_t>(floorv);
    else {
        const int64_t f = static_cast<int64_t>(floorv);
        q = (f & 1) ? f + 1 : f;  // exact .5 → nearest even integer
    }
    if (q > 8388607) q = 8388607;
    if (q < -8388608) q = -8388608;
    return static_cast<int32_t>(q);
}

bool write_wav_stereo(const std::string& path, StereoBuffer& buf, SampleFormat fmt, std::string& err) {
    const uint32_t frame_bytes = (fmt == SampleFormat::Pcm16) ? 4u : (fmt == SampleFormat::Pcm24 ? 6u : 8u);
    const uint32_t data_bytes = static_cast<uint32_t>(buf.frames() * frame_bytes);  // 2ch interleaved
    std::vector<uint8_t> header;
    header.reserve(fmt == SampleFormat::F32 ? 58u : 44u);
    const char* riff = "RIFF"; const char* wave = "WAVE"; const char* fmt_id = "fmt "; const char* data_id = "data";
    header.insert(header.end(), riff, riff + 4);
    put_u32_le(header, (fmt == SampleFormat::F32 ? 50u : 36u) + data_bytes);
    header.insert(header.end(), wave, wave + 4);
    header.insert(header.end(), fmt_id, fmt_id + 4);
    put_u32_le(header, fmt == SampleFormat::F32 ? 18u : 16u);  // fmt chunk size (18 = plain fmt + cbSize)
    put_u16_le(header, fmt == SampleFormat::F32 ? 3u : 1u);    // audio format: PCM / IEEE_FLOAT (not extensible)
    put_u16_le(header, 2u);                                    // channels
    put_u32_le(header, static_cast<uint32_t>(buf.sample_rate));
    put_u32_le(header, static_cast<uint32_t>(buf.sample_rate) * frame_bytes);  // byte rate
    put_u16_le(header, static_cast<uint16_t>(frame_bytes));    // block align
    put_u16_le(header, fmt == SampleFormat::Pcm16 ? 16u : (fmt == SampleFormat::Pcm24 ? 24u : 32u));
    if (fmt == SampleFormat::F32) {
        put_u16_le(header, 0u);                                // cbSize = 0
        header.insert(header.end(), "fact", "fact" + 4);
        put_u32_le(header, 4u);                                // fact chunk size
        put_u32_le(header, static_cast<uint32_t>(buf.frames()));  // dwSampleLength = frames per channel
    }
    header.insert(header.end(), data_id, data_id + 4);
    put_u32_le(header, data_bytes);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open for write '" + path + "'"; return false; }
    bool ok = std::fwrite(header.data(), 1, header.size(), f) == header.size();
    // Convert and write in chunks (buffer is float interleaved; no giant allocation).
    static constexpr size_t kChunkFrames = 4096;
    const size_t frames = buf.frames();
    if (fmt == SampleFormat::Pcm16) {
        std::vector<int16_t> pcm(kChunkFrames * 2);
        for (size_t base = 0; ok && base < frames; base += kChunkFrames) {
            const size_t n = (frames - base < kChunkFrames) ? (frames - base) : kChunkFrames;
            for (size_t i = 0; i < n * 2; ++i) pcm[i] = float_to_pcm16(buf.interleaved[(base * 2) + i]);
            ok = std::fwrite(pcm.data(), sizeof(int16_t), n * 2, f) == n * 2;
        }
    } else if (fmt == SampleFormat::Pcm24) {
        std::vector<uint8_t> bytes(kChunkFrames * 6);
        for (size_t base = 0; ok && base < frames; base += kChunkFrames) {
            const size_t n = (frames - base < kChunkFrames) ? (frames - base) : kChunkFrames;
            for (size_t i = 0; i < n * 2; ++i) {
                const int32_t v = float_to_pcm24(buf.interleaved[(base * 2) + i]);
                bytes[i * 3 + 0] = static_cast<uint8_t>(v & 0xFF);
                bytes[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);   // arithmetic shift, masked
                bytes[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            }
            ok = std::fwrite(bytes.data(), 1, n * 6, f) == n * 6;
        }
    } else {  // F32: bit pattern copied verbatim (no scaling, no clipping, NaN payloads preserved).
        std::vector<uint8_t> bytes(kChunkFrames * 2 * 4);
        for (size_t base = 0; ok && base < frames; base += kChunkFrames) {
            const size_t n = (frames - base < kChunkFrames) ? (frames - base) : kChunkFrames;
            for (size_t i = 0; i < n * 2; ++i) {
                const float x = buf.interleaved[(base * 2) + i];
                uint32_t u;
                std::memcpy(&u, &x, sizeof(u));
                bytes[i * 4 + 0] = static_cast<uint8_t>(u & 0xFFu);
                bytes[i * 4 + 1] = static_cast<uint8_t>((u >> 8) & 0xFFu);
                bytes[i * 4 + 2] = static_cast<uint8_t>((u >> 16) & 0xFFu);
                bytes[i * 4 + 3] = static_cast<uint8_t>((u >> 24) & 0xFFu);
            }
            ok = std::fwrite(bytes.data(), 1, n * 8, f) == n * 8;
        }
    }
    if (std::fclose(f) != 0) ok = false;
    if (!ok) err = "write failed '" + path + "'";
    return ok;
}

bool write_pcm16_stereo(const std::string& path, StereoBuffer& buf, std::string& err) {
    return write_wav_stereo(path, buf, SampleFormat::Pcm16, err);
}

ReadResult read_wav(const std::string& path) {
    ReadResult r;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { r.error = "cannot open '" + path + "'"; return r; }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 44) { std::fclose(f); r.error = "file too small for canonical WAV"; return r; }
    std::vector<uint8_t> all(static_cast<size_t>(size));
    if (std::fread(all.data(), 1, all.size(), f) != all.size()) { std::fclose(f); r.error = "short read"; return r; }
    std::fclose(f);

    if (std::memcmp(all.data(), "RIFF", 4) != 0 || std::memcmp(all.data() + 8, "WAVE", 4) != 0) {
        r.error = "not a RIFF/WAVE file"; return r;
    }
    size_t pos = 12;
    uint16_t fmt_tag = 0, channels = 0, bits = 0;
    uint32_t sr = 0;
    const uint8_t* data = nullptr;
    uint32_t data_len = 0;
    while (pos + 8 <= all.size()) {
        const char* id = reinterpret_cast<const char*>(all.data() + pos);
        const uint32_t sz = rd_u32_le(all.data() + pos + 4);
        if (pos + 8 + sz > all.size()) break;  // truncated chunk: stop scanning
        if (std::memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            fmt_tag = rd_u16_le(all.data() + pos + 8);
            channels = rd_u16_le(all.data() + pos + 10);
            sr = rd_u32_le(all.data() + pos + 12);
            bits = rd_u16_le(all.data() + pos + 22);
        } else if (std::memcmp(id, "data", 4) == 0) {
            data = all.data() + pos + 8;
            data_len = sz;
        }
        pos += 8 + sz + (sz & 1u);  // chunks are word-aligned
    }
    if (!data || fmt_tag == 0) { r.error = "missing fmt/data chunk"; return r; }
    if (channels != 1 && channels != 2) { r.error = "only mono/stereo supported"; return r; }
    if (fmt_tag == 1 && bits != 16 && bits != 24) { r.error = "PCM must be 16- or 24-bit"; return r; }
    // WP-0125 fuzz: without these, tag-3 files with bits∈{8,24} read out of bounds and bits=0
    // divides by zero (silently on aarch64) — strict fmt validation is the whole defense.
    if (fmt_tag == 3 && bits != 32) { r.error = "float WAV must be 32-bit"; return r; }
    if (sr == 0 || sr > 1000000u) { r.error = "implausible sample rate"; return r; }
    if (fmt_tag != 1 && fmt_tag != 3) { r.error = "unsupported WAV format tag"; return r; }

    const size_t bps = static_cast<size_t>(channels) * (bits / 8u);
    const size_t nframes = data_len / bps;
    r.sample_rate = static_cast<int>(sr);
    r.channels = channels;
    r.samples.resize(nframes * channels);
    for (size_t i = 0; i < nframes * channels; ++i) {
        if (fmt_tag == 1 && bits == 16) {
            const int16_t v = static_cast<int16_t>(rd_u16_le(data + i * 2));
            r.samples[i] = static_cast<float>(v) * (1.0f / 32768.0f);
        } else if (fmt_tag == 1) {  // bits == 24 (validated above): explicit 3-byte LE + sign extension,
                                    // never rd_u32_le here — that would read one byte past the chunk end.
            const uint8_t* p = data + i * 3;
            int32_t v = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) |
                        (static_cast<int32_t>(p[2]) << 16);
            if (v & 0x800000) v -= 0x1000000;
            r.samples[i] = static_cast<float>(v) * (1.0f / 8388608.0f);
        } else {  // float32
            uint32_t u = rd_u32_le(data + i * 4);
            float v;
            std::memcpy(&v, &u, sizeof(v));
            r.samples[i] = v;
        }
    }
    r.ok = true;
    return r;
}

}  // namespace op::wav
