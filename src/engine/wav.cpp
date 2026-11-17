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

bool write_pcm16_stereo(const std::string& path, StereoBuffer& buf, std::string& err) {
    const uint32_t data_bytes = static_cast<uint32_t>(buf.frames() * 4u);  // 2ch * int16
    std::vector<uint8_t> header;
    header.reserve(44);
    const char* riff = "RIFF"; const char* wave = "WAVE"; const char* fmt = "fmt "; const char* data = "data";
    header.insert(header.end(), riff, riff + 4);
    put_u32_le(header, 36u + data_bytes);
    header.insert(header.end(), wave, wave + 4);
    header.insert(header.end(), fmt, fmt + 4);
    put_u32_le(header, 16u);              // PCM fmt chunk size
    put_u16_le(header, 1u);               // audio format = PCM
    put_u16_le(header, 2u);               // channels
    put_u32_le(header, static_cast<uint32_t>(buf.sample_rate));
    put_u32_le(header, static_cast<uint32_t>(buf.sample_rate) * 4u);  // byte rate
    put_u16_le(header, 4u);               // block align
    put_u16_le(header, 16u);              // bits
    header.insert(header.end(), data, data + 4);
    put_u32_le(header, data_bytes);

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open for write '" + path + "'"; return false; }
    bool ok = std::fwrite(header.data(), 1, header.size(), f) == header.size();
    // Convert in place to int16 stream (buffer is float interleaved).
    static constexpr size_t kChunkFrames = 4096;
    std::vector<int16_t> pcm(kChunkFrames * 2);
    const size_t frames = buf.frames();
    for (size_t base = 0; ok && base < frames; base += kChunkFrames) {
        const size_t n = (frames - base < kChunkFrames) ? (frames - base) : kChunkFrames;
        for (size_t i = 0; i < n * 2; ++i) pcm[i] = float_to_pcm16(buf.interleaved[(base * 2) + i]);
        ok = std::fwrite(pcm.data(), sizeof(int16_t), n * 2, f) == n * 2;
    }
    if (std::fclose(f) != 0) ok = false;
    if (!ok) err = "write failed '" + path + "'";
    return ok;
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
    if (fmt_tag == 1 && bits != 16) { r.error = "PCM must be 16-bit in v0"; return r; }
    if (fmt_tag != 1 && fmt_tag != 3) { r.error = "unsupported WAV format tag"; return r; }

    const size_t bps = static_cast<size_t>(channels) * (bits / 8u);
    const size_t nframes = data_len / bps;
    r.sample_rate = static_cast<int>(sr);
    r.channels = channels;
    r.samples.resize(nframes * channels);
    for (size_t i = 0; i < nframes * channels; ++i) {
        if (fmt_tag == 1) {
            const int16_t v = static_cast<int16_t>(rd_u16_le(data + i * 2));
            r.samples[i] = static_cast<float>(v) * (1.0f / 32768.0f);
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
