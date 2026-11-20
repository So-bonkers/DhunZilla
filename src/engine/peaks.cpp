// engine/peaks.cpp — see peaks.hpp / docs/spec/peaks.md (WP-0171).
// No libm of any kind: only comparisons, integer arithmetic and stdio formatting.
#include "engine/peaks.hpp"

#include <cstdio>
#include <cstring>

namespace op::peaks {

// Non-finite samples (NaN, +-inf, or magnitude beyond 1e30) count as 0.0f (spec §Definitions).
// NaN fails every comparison; +-inf and huge magnitudes are caught by the bounds test. No libm.
static float sanitize(float x) {
    if (!(x >= -1e30 && x <= 1e30)) return 0.0f;  // false for NaN, +-inf and |x| > 1e30 (double bounds)
    return x;
}

Peaks compute_peaks(const std::vector<float>& interleaved, int channels, int sample_rate, int bins) {
    Peaks p;
    p.sample_rate = sample_rate;
    p.channels = channels;
    if ((channels != 1 && channels != 2) || bins < 1 || bins > kMaxBins) return p;  // bins == 0 (spec)

    const long long frames = static_cast<long long>(interleaved.size()) / channels;
    p.frames = frames;
    p.bins = bins;
    p.min.assign(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(bins), 0.0f));
    p.max.assign(static_cast<size_t>(channels), std::vector<float>(static_cast<size_t>(bins), 0.0f));
    if (frames == 0) return p;  // every value stays 0.0f (spec)

    for (int b = 0; b < bins; ++b) {
        long long start = static_cast<long long>(b) * frames / bins;
        long long end = static_cast<long long>(b + 1) * frames / bins;
        if (end == start) end = start + 1;  // fewer frames than bins: repeat the frame (spec)
        for (int c = 0; c < channels; ++c) {
            float lo = sanitize(interleaved[static_cast<size_t>(start * channels + c)]);
            float hi = lo;
            for (long long f = start + 1; f < end; ++f) {
                const float v = sanitize(interleaved[static_cast<size_t>(f * channels + c)]);
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            p.min[static_cast<size_t>(c)][static_cast<size_t>(b)] = lo;
            p.max[static_cast<size_t>(c)][static_cast<size_t>(b)] = hi;
        }
    }
    return p;
}

// One value with %.6f; the text "-0.000000" prints "0.000000" (spec §Definitions).
static void append_value(std::string& out, float v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.6f", static_cast<double>(v));
    if (std::strcmp(buf, "-0.000000") == 0) out += "0.000000";
    else out += buf;
}

static void append_rows(std::string& out, const std::vector<std::vector<float>>& rows) {
    out += '[';
    for (size_t c = 0; c < rows.size(); ++c) {
        if (c != 0) out += ',';
        out += '[';
        const std::vector<float>& row = rows[c];
        for (size_t i = 0; i < row.size(); ++i) {
            if (i != 0) out += ',';
            append_value(out, row[i]);
        }
        out += ']';
    }
    out += ']';
}

std::string to_json(const Peaks& p) {
    std::string out;
    size_t values = 0;
    for (const auto& row : p.min) values += row.size();
    for (const auto& row : p.max) values += row.size();
    out.reserve(96 + values * 10);
    char head[128];
    std::snprintf(head, sizeof head, "{\"version\":1,\"sample_rate\":%d,\"channels\":%d,\"frames\":%lld,\"bins\":%d,",
                  p.sample_rate, p.channels, p.frames, p.bins);
    out += head;
    out += "\"min\":";
    append_rows(out, p.min);
    out += ",\"max\":";
    append_rows(out, p.max);
    out += "}\n";
    return out;
}

}  // namespace op::peaks
