// engine/meters.cpp — per-track and master peak/RMS dBFS + integrated LUFS (WP-0194), plus the
// WP-0243 per-bar peak/RMS arrays (--per-bar). This module adds NO DSP of its own: every measurement
// is op::analyze::analyze_samples over a buffer that op::render_stems produced in memory; every bar
// window is op::sample_at_bar, the renderer's own bar→sample mapping (tempo map + time signature).
// No file I/O, no project mutation, no direct libm calls (only snprintf, std::ceil and + - * / here;
// the dB math lives in analyze.cpp). Byte-exact JSON per spec.

#include "engine/meters.hpp"

#include <cmath>
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

// WP-0243: the "bars": [...] key in full (one line; bar objects separated by ", "), no leading comma.
// A bar object is exactly {"bar": i, "start_sample": s, "end_sample": e, "peak_dbfs": n|null,
// "rms_dbfs": n|null} — same %.4f / -0.0000 rules as everything else here.
std::string bars_json(const std::vector<BarMeter>& bars) {
    char buf[64];
    std::string s = "\"bars\": [";
    for (size_t i = 0; i < bars.size(); ++i) {
        if (i > 0) s += ", ";
        s += "{\"bar\": ";
        std::snprintf(buf, sizeof buf, "%lld", bars[i].bar); s += buf;
        s += ", \"start_sample\": ";
        std::snprintf(buf, sizeof buf, "%lld", bars[i].start_sample); s += buf;
        s += ", \"end_sample\": ";
        std::snprintf(buf, sizeof buf, "%lld", bars[i].end_sample); s += buf;
        s += ", \"peak_dbfs\": " + fmt_opt(bars[i].peak_dbfs);
        s += ", \"rms_dbfs\": " + fmt_opt(bars[i].rms_dbfs);
        s += "}";
    }
    s += "]";
    return s;
}

// WP-0243: one BarMeter per bar 0..bar_count-1 of `buf`, windows straight from op::sample_at_bar —
// the renderer's mapping, so the tempo map and time signature fall out on their own. The last bar of
// a partial (non-integer length_bars) project ends at total_samples_for(p) == Report::frames because
// sample_at_bar clamps bars >= length_bars.
void fill_bars(const Project& p, const wav::StereoBuffer& buf, long long bar_count,
               std::vector<BarMeter>& into) {
    into.reserve(static_cast<size_t>(bar_count));
    for (long long bar = 0; bar < bar_count; ++bar) {
        const long long start_sample = op::sample_at_bar(p, static_cast<double>(bar));
        const long long end_sample = op::sample_at_bar(p, static_cast<double>(bar + 1));
        into.push_back(meter_bar(buf, bar, start_sample, end_sample));
    }
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

BarMeter meter_bar(const wav::StereoBuffer& buf, long long bar, long long start_sample, long long end_sample) {
    BarMeter b;
    b.bar = bar;
    b.start_sample = start_sample;
    b.end_sample = end_sample;
    // Precondition 0 <= start_sample <= end_sample <= buf.frames() (spec). Clamp defensively anyway —
    // a caller bug must never read past the buffer.
    const long long frames = static_cast<long long>(buf.frames());
    if (start_sample < 0) start_sample = 0;
    if (end_sample > frames) end_sample = frames;
    if (start_sample >= end_sample) return b;  // empty range: both optionals stay empty (spec)
    // analyze_samples over exactly those frames (stereo, interleaved).
    std::vector<float> window(buf.interleaved.begin() + 2 * start_sample,
                              buf.interleaved.begin() + 2 * end_sample);
    const op::analyze::Features f = op::analyze::analyze_samples(window, 2, buf.sample_rate);
    b.peak_dbfs = f.peak_dbfs;  // copied unchanged — an all-silent window yields two empties
    b.rms_dbfs = f.rms_dbfs;
    return b;
}

bool measure_project_per_bar(const Project& p, const std::string& base_dir, Report& out, std::string& err) {
    std::vector<wav::StereoBuffer> stems;  // render_stems sizes and resets these
    wav::StereoBuffer mix;
    if (!op::render_stems(p, base_dir, stems, mix, err)) return false;  // err stays exactly as set

    out = Report{};  // a fresh Report on success (spec)
    out.sample_rate = mix.sample_rate;
    out.frames = static_cast<long long>(mix.frames());
    if (out.sample_rate > 0)
        out.duration_s = static_cast<double>(out.frames) / static_cast<double>(out.sample_rate);
    out.per_bar = true;
    out.bar_count = static_cast<long long>(std::ceil(p.length_bars));  // the bar count opd timeline prints
    out.tracks.reserve(p.tracks.size());
    for (size_t i = 0; i < p.tracks.size(); ++i) {
        TrackMeter tm;
        tm.name = p.tracks[i].name;  // inaudible (muted / un-soloed) tracks have all-zero stems → nulls
        tm.meter = meter_buffer(stems[i]);
        fill_bars(p, stems[i], out.bar_count, tm.bars);
        out.tracks.push_back(std::move(tm));
    }
    out.master = meter_buffer(mix);  // the mix as render_stems returns it: master_gain_db already applied
    fill_bars(p, mix, out.bar_count, out.master_bars);
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
    if (r.per_bar) {  // WP-0243: only new top-level key; without the flag nothing here changes
        std::snprintf(buf, sizeof buf, "  \"bar_count\": %lld,\n", r.bar_count);
        s += buf;
    }
    if (r.tracks.empty()) {
        s += "  \"tracks\": [],\n";
    } else {
        s += "  \"tracks\": [\n";
        for (size_t i = 0; i < r.tracks.size(); ++i) {
            std::snprintf(buf, sizeof buf, "    {\"index\": %zu, ", i);
            s += buf;
            s += "\"name\": " + json_string(r.tracks[i].name) + ", ";
            s += meter_fields(r.tracks[i].meter);
            if (r.per_bar) s += ", " + bars_json(r.tracks[i].bars);
            s += (i + 1 < r.tracks.size() ? "},\n" : "}\n");
        }
        s += "  ],\n";
    }
    s += "  \"master\": {" + meter_fields(r.master);
    if (r.per_bar) s += ", " + bars_json(r.master_bars);
    s += "}\n";
    s += "}\n";
    return s;
}

}  // namespace op::meters
