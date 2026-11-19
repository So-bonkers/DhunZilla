// engine/analyze.hpp — objective audio features (peak/RMS dBFS, BS.1770-4 integrated LUFS,
// spectral centroid) as canonical JSON. Spec: docs/spec/analyze.md (WP-0144).
//
// Example:
//   op::wav::ReadResult rr = op::wav::read_wav("render.wav");
//   op::analyze::Features f = op::analyze::analyze_samples(rr.samples, rr.channels, rr.sample_rate);
//   std::fputs(op::analyze::to_json(f).c_str(), stdout);  // byte-exact deterministic text
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace op::analyze {
struct Features {
    int sample_rate = 0;
    int channels = 0;                                   // 1 or 2
    long long frames = 0;                               // samples per channel
    double duration_s = 0.0;                            // frames / sample_rate
    std::vector<std::optional<double>> peak_dbfs_per_channel;  // size == channels; nullopt = silent channel
    std::optional<double> peak_dbfs;                    // max over channels
    std::optional<double> rms_dbfs;
    std::optional<double> integrated_lufs;
    std::optional<double> spectral_centroid_hz;
};
// interleaved float samples in [-1,1] (non-finite samples are treated as 0.0). Precondition:
// channels in {1,2}, sample_rate in [8000, 192000] (the CLI enforces this; analyze_samples need not).
Features analyze_samples(const std::vector<float>& interleaved, int channels, int sample_rate);
// Canonical JSON text, byte-exact format below, ending in "}\n".
std::string to_json(const Features& f);
}  // namespace op::analyze
