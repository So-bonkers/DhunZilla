// engine/loudness.hpp — mastering-grade loudness measurements: true peak (dBTP, 4x oversampled),
// momentary / short-term maximum LUFS and EBU Tech 3342 loudness range. Spec: docs/spec/loudness.md
// (WP-0168). Deterministic: same samples in -> byte-identical JSON text out; all arithmetic in double,
// libm only via the op::dsp wrappers (ADR-0004).
//
// Usage:
//   #include "engine/loudness.hpp"
//   auto rr = op::wav::read_audio("mix.wav");
//   const op::loudness::Result r = op::loudness::measure(rr.samples, rr.channels, rr.sample_rate);
//   std::fputs(op::loudness::to_json(r).c_str(), stdout);  // {"sample_rate":48000,...,}\n
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace op::loudness {
struct Result {
    int sample_rate = 0;
    int channels = 0;                          // 1 or 2
    long long frames = 0;                      // samples per channel
    double duration_s = 0.0;                   // frames / sample_rate
    std::optional<double> true_peak_dbtp;
    std::optional<double> momentary_max_lufs;
    std::optional<double> short_term_max_lufs;
    std::optional<double> loudness_range_lu;
};
// interleaved float samples (non-finite samples are treated as 0.0). channels in {1,2}.
Result measure(const std::vector<float>& interleaved, int channels, int sample_rate);
std::string to_json(const Result& r);          // canonical text below, ending in "}\n"
}  // namespace op::loudness
