// engine/meters.cpp — per-track and master peak/RMS dBFS + integrated LUFS (WP-0194).
// This module adds NO DSP of its own: every measurement is op::analyze::analyze_samples over a buffer
// that op::render_stems produced in memory. No file I/O, no project mutation, no direct libm calls
// (only snprintf and + - * / here; the dB math lives in analyze.cpp). Byte-exact JSON per spec.

#include "engine/meters.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/analyze.hpp"
#include "engine/render.hpp"

namespace op::meters {
namespace {

// %.4f with the spec's negative-zero rule: "-0.0000" is printed as "0.0000".
std::string fmt4(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    if (std::strcmp(buf, "-0.0000") == 0) return "0.0000";
    return std::string(buf);
}

std::string fmt_opt(const std::optional<double>& v) { return v ? fmt4(*v) : std::string("null"); }

// JSON string per spec: '"' -> \" , '\' -> \\ , every byte < 0x20 -> \u00XX (4 lowercase hex digits);
// all other bytes (including UTF-8 sequences) verbatim.
std::string json_string(const std::string& s) {
    std::string out;
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (c < 0x20u) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
    return out;
}

// The meter object's three fields, no braces: "peak_dbfs": <n>, "rms_dbfs": <n>, "integrated_lufs": <n>.
std::string meter_fields(const Meter& m) {
    return "\"peak_dbfs\": " + fmt_opt(m.peak_dbfs) + ", \"rms_dbfs\": " + fmt_opt(m.rms_dbfs) +
           ", \"integrated_lufs\": " + fmt_opt(m.integrated_lufs);
}

}  // namespace

Meter meter_buffer(const wav::StereoBuffer& buf) {
    Meter m;
    if (buf.frames() == 0) return m;  // zero frames: all three optionals stay empty (spec)
    const op::analyze::Features f = op::analyze::analyze_samples(buf.interleaved, 2, buf.sample_rate);
    m.peak_dbfs = f.peak_dbfs;            // copied unchanged — an all-zero buffer yields three empties
    m.rms_dbfs = f.rms_dbfs;
    m.integrated_lufs = f.integrated_lufs;
    return m;
}

bool measure_project(const Project& p, const std::string& base_dir, Report& out, std::string& err) {
    std::vector<wav::StereoBuffer> stems;  // render_stems sizes and resets these
    wav::StereoBuffer mix;
    if (!op::render_stems(p, base_dir, stems, mix, err)) return false;  // err stays exactly as set

    out = Report{};  // a fresh Report on success (spec)
    out.sample_rate = mix.sample_rate;
    out.frames = static_cast<long long>(mix.frames());
    if (out.sample_rate > 0)
        out.duration_s = static_cast<double>(out.frames) / static_cast<double>(out.sample_rate);
    out.tracks.reserve(p.tracks.size());
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        TrackMeter tm;
        tm.name = p.tracks[i].name;  // inaudible (muted / un-soloed) tracks have all-zero stems → nulls
        tm.meter = meter_buffer(stems[i]);
        out.tracks.push_back(std::move(tm));
    }
    out.master = meter_buffer(mix);  // the mix as render_stems returns it: master_gain_db already applied
    return true;
}

std::string to_json(const Report& r) {
    char buf[64];
    std::string s;
    s += "{\n";
    std::snprintf(buf, sizeof buf, "  \"sample_rate\": %d,\n", r.sample_rate);
    s += buf;
    std::snprintf(buf, sizeof buf, "  \"frames\": %lld,\n", r.frames);
    s += buf;
    std::snprintf(buf, sizeof buf, "  \"duration_s\": %.6f,\n", r.duration_s);
    s += buf;
    if (r.tracks.empty()) {
        s += "  \"tracks\": [],\n";
    } else {
        s += "  \"tracks\": [\n";
        for (size_t i = 0; i < r.tracks.size(); ++i) {
            std::snprintf(buf, sizeof buf, "    {\"index\": %zu, ", i);
            s += buf;
            s += "\"name\": " + json_string(r.tracks[i].name) + ", ";
            s += meter_fields(r.tracks[i].meter);
            s += (i + 1 < r.tracks.size() ? "},\n" : "}\n");
        }
        s += "  ],\n";
    }
    s += "  \"master\": {" + meter_fields(r.master) + "}\n";
    s += "}\n";
    return s;
}

}  // namespace op::meters
