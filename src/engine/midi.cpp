// engine/midi.cpp — see midi.hpp. Boring explicit byte parsing; every read is bounds-checked.
#include "engine/midi.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <vector>

#include "engine/dsp.hpp"

namespace op {
namespace {

constexpr size_t kMaxMidiBytes = 32ull * 1024 * 1024;

struct Reader {
    const std::vector<uint8_t>& b;
    size_t pos = 0;
    bool fail = false;

    explicit Reader(const std::vector<uint8_t>& buf) : b(buf) {}

    bool take(size_t n) {  // advance window
        if (fail || pos + n > b.size()) { fail = true; return false; }
        pos += n;
        return true;
    }
    uint8_t u8() { if (fail || pos >= b.size()) { fail = true; return 0; } return b[pos++]; }
    uint32_t be32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | u8();
        return v;
    }
    uint16_t be16() {
        uint16_t v = 0;
        for (int i = 0; i < 2; ++i) v = static_cast<uint16_t>((v << 8) | u8());
        return v;
    }
    // Variable-length quantity (max 4 bytes, 7 bits each).
    uint32_t vlq() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            const uint8_t c = u8();
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
        fail = true;
        return 0;
    }
};

std::string sanitize_name(const std::string& in) {
    std::string out;
    for (unsigned char c : in)
        if (c >= 0x20 && c < 0x7F) out += static_cast<char>(c);
    return out;
}

struct OpenNote {
    long long tick = 0;
    int vel = 90;  // MIDI on-velocity, captured for Note.velocity (WP-0111)
};

struct TrackAccum {
    std::string name;
    std::vector<Note> notes;               // finished, unsorted
    std::map<int /*key*/, OpenNote> open;  // per channel-less: one op track per MIDI track (v0)
    long long last_tick = 0;
};

}  // namespace

// ---------------------------------------------------------------- export (SMF writer)

namespace {

constexpr uint16_t kExportPpq = 480;

void put_be32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 24; i >= 0; i -= 8) o.push_back(static_cast<uint8_t>((v >> i) & 0xFF));
}
void put_be16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(static_cast<uint8_t>(v >> 8));
    o.push_back(static_cast<uint8_t>(v & 0xFF));
}
void put_vlq(std::vector<uint8_t>& o, uint32_t v) {
    int nbytes = 1;
    while (nbytes < 4 && (v >> (7 * nbytes))) ++nbytes;
    if (v >> 28) v = (1u << 28) - 1;  // clamp to VLQ max rather than emit garbage
    for (int i = nbytes - 1; i > 0; --i) o.push_back(static_cast<uint8_t>(((v >> (7 * i)) & 0x7F) | 0x80));
    o.push_back(static_cast<uint8_t>(v & 0x7F));
}

// Nearest MIDI number for a bare frequency — table scan, no log() needed (dsp::powf_ only).
int nearest_midi(float f) {
    if (!(f > 0.0f)) return 69;
    int best = 69;
    float best_d = 1e30f;
    for (int m = 0; m < 128; ++m) {
        const float fm = 440.0f * dsp::powf_(2.0f, static_cast<float>(m - 69) / 12.0f);
        const float d = (fm > f) ? fm - f : f - fm;
        if (d < best_d) { best_d = d; best = m; }
    }
    return best;
}

struct MidiEvent {
    long long tick;
    int order;  // at equal ticks: OFF(0) before ON(1) — prevents stuck-note files
    std::vector<uint8_t> bytes;
};

}  // namespace

bool save_midi(const std::string& path, const Project& p, std::string& err) {
    if (p.tracks.empty()) { err = "midi export: project has no tracks"; return false; }
    if (!(p.bpm > 0.0)) { err = "midi export: bpm must be > 0"; return false; }

    std::vector<uint8_t> o;
    const uint16_t ntrks = static_cast<uint16_t>(p.tracks.size() + 1);
    o.insert(o.end(), {'M', 'T', 'h', 'd'});
    put_be32(o, 6);
    put_be16(o, 1);  // format 1
    put_be16(o, ntrks);
    put_be16(o, kExportPpq);

    auto emit_track = [&](std::vector<uint8_t>& body) {
        o.insert(o.end(), {'M', 'T', 'r', 'k'});
        put_be32(o, static_cast<uint32_t>(body.size()));
        o.insert(o.end(), body.begin(), body.end());
    };

    // Track 0: tempo + 4/4 marker.
    {
        std::vector<uint8_t> body;
        const uint32_t us_per_qn = static_cast<uint32_t>(std::llround(6.0e7 / p.bpm));
        put_vlq(body, 0);
        body.insert(body.end(), {0xFF, 0x51, 0x03,
                                 static_cast<uint8_t>((us_per_qn >> 16) & 0xFF),
                                 static_cast<uint8_t>((us_per_qn >> 8) & 0xFF),
                                 static_cast<uint8_t>(us_per_qn & 0xFF)});
        put_vlq(body, 0);
        body.insert(body.end(), {0xFF, 0x58, 0x04,
                                 static_cast<uint8_t>(std::min(255, std::max(1, p.beats_per_bar))),
                                 0x02, 0x24, 0x08});
        put_vlq(body, 0);
        body.insert(body.end(), {0xFF, 0x2F, 0x00});
        emit_track(body);
    }

    for (size_t ti = 0; ti < p.tracks.size(); ++ti) {
        const Track& tr = p.tracks[ti];
        std::vector<MidiEvent> evs;
        for (const Pattern& pat : p.patterns) {  // gather every pattern belonging to this track
            if (pat.track != static_cast<int>(ti)) continue;
            for (const Note& n : pat.notes) {
                const int midi = n.midi_note >= 0 ? n.midi_note : nearest_midi(tr.voice.freq_hz);
                int out_vel = static_cast<int>(std::llround(n.velocity * 127.0));  // WP-0111: dynamics carry out
                if (out_vel < 1) out_vel = 1;
                if (out_vel > 127) out_vel = 127;
                long long t1 = std::llround(n.t_beats * kExportPpq);
                long long t2 = std::llround((n.t_beats + n.dur_beats) * kExportPpq);
                if (t1 < 0) t1 = 0;
                if (t2 <= t1) t2 = t1 + 1;
                evs.push_back({t1, 1, {0x90, static_cast<uint8_t>(midi), static_cast<uint8_t>(out_vel)}});
                evs.push_back({t2, 0, {0x80, static_cast<uint8_t>(midi), 0x40}});
            }
        }
        std::sort(evs.begin(), evs.end(), [](const MidiEvent& a, const MidiEvent& b) {
            return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
        });

        std::vector<uint8_t> body;
        const std::string name = sanitize_name(tr.name);
        if (!name.empty()) {
            put_vlq(body, 0);
            body.push_back(0xFF); body.push_back(0x03);
            put_vlq(body, static_cast<uint32_t>(name.size()));
            body.insert(body.end(), name.begin(), name.end());
        }
        long long prev = 0;
        for (const auto& e : evs) {
            const long long delta = std::max(0LL, e.tick - prev);
            put_vlq(body, static_cast<uint32_t>(delta));
            body.insert(body.end(), e.bytes.begin(), e.bytes.end());
            prev += delta;
        }
        put_vlq(body, 0);
        body.insert(body.end(), {0xFF, 0x2F, 0x00});
        emit_track(body);
    }

    std::ofstream f(path, std::ios::binary);
    if (!f) { err = "midi export: cannot write '" + path + "'"; return false; }
    f.write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
    return f.good();
}

bool load_midi(const std::string& path, Project& out, std::string& err) {
    out = Project{};
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open '" + path + "'"; return false; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() > kMaxMidiBytes) { err = "midi: file exceeds 32 MB cap"; return false; }

    Reader r(buf);
    if (r.be32() != 0x4D546864u) { err = "midi: missing MThd magic"; return false; }  // "MThd"
    const uint32_t hdrlen = r.be32();
    if (hdrlen < 6) { err = "midi: MThd length < 6"; return false; }
    const uint16_t format = r.be16();
    const uint16_t ntrks = r.be16();
    const uint16_t division = r.be16();
    if (format != 0 && format != 1) { err = "midi: only SMF format 0/1 supported (got " + std::to_string(format) + ")"; return false; }
    if (division & 0x8000) { err = "midi: SMPTE division not supported — export ticks-per-quarter-note (480 typical)"; return false; }
    const double ppq = static_cast<double>(division);
    if (ppq <= 0.0) { err = "midi: zero ppq"; return false; }
    r.take(hdrlen - 6);  // skip any extended header bytes

    Project p;
    p.sample_rate = 48000;
    p.beats_per_bar = 4;
    double bpm = 120.0;
    bool tempo_seen = false;
    std::vector<TrackAccum> accums(ntrks);

    for (uint16_t trk = 0; trk < ntrks && !r.fail; ++trk) {
        // "MTrk"
        const uint32_t magic = r.be32();
        if (magic != 0x4D54726Bu) { err = "midi: expected MTrk chunk at track " + std::to_string(trk); return false; }
        const uint32_t len = r.be32();
        const size_t trk_end = r.pos + len;
        if (trk_end > buf.size()) { err = "midi: MTrk length past end of file"; return false; }

        long long tick = 0;
        uint8_t running = 0;
        while (r.pos < trk_end && !r.fail) {
            tick += static_cast<long long>(r.vlq());
            if (r.fail) break;
            uint8_t status = r.u8();
            if (status < 0x80) {  // running status: byte is data of previous event
                if (running == 0) { err = "midi: data byte without status in track " + std::to_string(trk); return false; }
                r.pos--;
                status = running;
            } else {
                running = (status < 0xF0) ? status : 0;
            }

            if (status == 0xFF) {  // meta event
                const uint8_t mtype = r.u8();
                const uint32_t mlen = r.vlq();
                const size_t m_end = r.pos + mlen;
                if (m_end > trk_end) { err = "midi: meta event past track end"; return false; }
                if (mtype == 0x51 && mlen >= 3) {
                    const uint32_t t24 = (static_cast<uint32_t>(r.u8()) << 16) |
                                         (static_cast<uint32_t>(r.u8()) << 8) | r.u8();
                    if (!tempo_seen && t24 > 0) { bpm = 6.0e7 / static_cast<double>(t24); tempo_seen = true; }
                } else if (mtype == 0x03) {
                    std::string nm;
                    for (uint32_t i = 0; i < mlen && r.pos < trk_end; ++i) nm += static_cast<char>(r.u8());
                    accums[trk].name = sanitize_name(nm);
                } else if (mtype == 0x2F) {
                    break;  // end of track: remaining open notes close at last_tick below
                }
                r.pos = m_end;  // skip any unread meta payload bytes exactly
                accums[trk].last_tick = tick;
            } else if (status == 0xF0 || status == 0xF7) {  // sysex: length-prefixed (F7) or terminated (F0)
                if (status == 0xF7) {
                    const uint32_t slen = r.vlq();
                    r.take(slen);
                } else {
                    while (!r.fail && r.u8() != 0xF7) {}
                }
                accums[trk].last_tick = tick;
            } else if ((status & 0xF0) == 0x90) {  // note on (velocity-0 == off per MIDI convention)
                const uint8_t key = r.u8();
                const uint8_t vel = r.u8();
                auto& acc = accums[trk];
                if (key > 127) { acc.last_tick = tick; continue; }  // invalid data byte: ignore event, keep parsing
                if (vel > 0) {
                    auto it = acc.open.find(key);
                    if (it != acc.open.end()) {  // retrigger: close previous at this tick, open new
                        const double t_beats = static_cast<double>(it->second.tick) / ppq;
                        const double dur = static_cast<double>(tick - it->second.tick) / ppq;
                        if (dur > 0.0) acc.notes.push_back({t_beats, dur, key, it->second.vel / 127.0});
                        acc.open.erase(it);
                    }
                    acc.open[key] = OpenNote{tick, vel};
                } else {
                    auto it = acc.open.find(key);
                    if (it != acc.open.end()) {
                        const double t_beats = static_cast<double>(it->second.tick) / ppq;
                        const double dur = static_cast<double>(tick - it->second.tick) / ppq;
                        if (dur > 0.0) acc.notes.push_back({t_beats, dur, key, it->second.vel / 127.0});
                        acc.open.erase(it);
                    }
                }
                acc.last_tick = tick;
            } else if ((status & 0xF0) == 0x80) {  // note off
                const uint8_t key = r.u8();
                r.u8();  // velocity (ignored)
                auto& acc = accums[trk];
                if (key > 127) { acc.last_tick = tick; continue; }  // invalid data byte: ignore event
                auto it = acc.open.find(key);
                if (it != acc.open.end()) {
                    const double t_beats = static_cast<double>(it->second.tick) / ppq;
                    const double dur = static_cast<double>(tick - it->second.tick) / ppq;
                    if (dur > 0.0) acc.notes.push_back({t_beats, dur, key, it->second.vel / 127.0});
                    acc.open.erase(it);
                }
                acc.last_tick = tick;
            } else if ((status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0) {
                r.u8();  // program / channel pressure: one operand
            } else {
                r.u8(); r.u8();  // control, pitch bend, poly AT: two operands
            }
        }
        if (r.fail) { err = "midi: truncated/corrupt event stream in track " + std::to_string(trk); return false; }
        r.pos = trk_end;  // resync past any trailing bytes in the chunk

        auto& acc = accums[trk];
        for (auto& kv : acc.open) {  // unclosed notes close at track's last event tick
            const double t_beats = static_cast<double>(kv.second.tick) / ppq;
            const double dur = static_cast<double>(acc.last_tick - kv.second.tick) / ppq;
            if (dur > 0.0) acc.notes.push_back({t_beats, dur, static_cast<uint8_t>(kv.first), kv.second.vel / 127.0});
        }
    }

    double max_end = 0.0;
    int out_idx = 0;
    for (auto& acc : accums) {
        if (acc.notes.empty()) continue;
        std::sort(acc.notes.begin(), acc.notes.end(), [](const Note& a, const Note& b) {
            return a.t_beats != b.t_beats ? a.t_beats < b.t_beats : a.midi_note < b.midi_note;
        });
        for (const Note& n : acc.notes) max_end = std::max(max_end, n.t_beats + n.dur_beats);
        Track t;
        t.name = acc.name.empty() ? ("MIDI " + std::to_string(out_idx)) : acc.name;
        t.voice.kind = VoiceKind::Synth;  // imported MIDI plays on the built-in pitched synth
        Pattern pat;
        pat.track = out_idx;
        pat.notes = std::move(acc.notes);
        p.tracks.push_back(std::move(t));
        p.patterns.push_back(std::move(pat));
        ++out_idx;
    }
    if (p.tracks.empty()) { err = "midi: file contains no playable note events"; return false; }

    p.bpm = bpm;
    p.length_bars = std::max(1.0, std::ceil(max_end / static_cast<double>(p.beats_per_bar)));
    out = std::move(p);
    return true;
}

}  // namespace op
