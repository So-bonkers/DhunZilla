// engine/peaks.hpp — min/max waveform peaks per bin for sample and bounce waveform drawing.
// Deterministic, no libm (WP-0171). Spec: docs/spec/peaks.md.
//
// Example:
//   op::wav::ReadResult rr = op::wav::read_audio("kick.wav");
//   op::peaks::Peaks p = op::peaks::compute_peaks(rr.samples, rr.channels, rr.sample_rate, 1024);
//   std::fputs(op::peaks::to_json(p).c_str(), stdout);  // one compact JSON line, ends with '\n'
#pragma once
#include <string>
#include <vector>
namespace op::peaks {
constexpr int kMaxBins = 65536;
struct Peaks {
    int sample_rate = 0;
    int channels = 0;                     // 1 or 2
    long long frames = 0;                 // samples per channel
    int bins = 0;
    std::vector<std::vector<float>> min;  // [channel][bin]
    std::vector<std::vector<float>> max;  // [channel][bin]
};
// Reduce interleaved float samples to `bins` min/max values per channel (docs/spec/peaks.md §Definitions).
// channels must be in {1,2} and bins in [1, kMaxBins]; otherwise the result has bins == 0, frames == 0 and
// empty min/max. Non-finite samples (NaN, +-inf, |x| > 1e30) count as 0.0f. Usage: see file header.
Peaks compute_peaks(const std::vector<float>& interleaved, int channels, int sample_rate, int bins);
// Canonical JSON text: one compact line ending in '\n', every value printed with %.6f; the text "-0.000000"
// is printed "0.000000". Format (docs/spec/peaks.md §Definitions):
//   {"version":1,"sample_rate":R,"channels":C,"frames":F,"bins":N,"min":[[v,...],...],"max":[[v,...],...]}\n
std::string to_json(const Peaks& p);
}  // namespace op::peaks
