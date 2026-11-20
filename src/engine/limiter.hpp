// engine/limiter.hpp — deterministic look-ahead brickwall peak limiter for the final mix (WP-0176).
// Fixed 5 ms look-ahead, one-pole attack/release in the dB domain, stereo-linked, no oversampling.
// Render-time only: no project-model key, no op. Spec: docs/spec/limiter.md. libm via op::dsp only.
//
// Example:
//   op::wav::StereoBuffer mix = ...;                 // the final rendered mix
//   op::limiter::Stats stats;
//   std::string err;
//   if (!op::limiter::apply(mix.interleaved, 2, mix.sample_rate, -1.0, stats, err)) { /* handle err */ }
//   // mix.interleaved now holds the limited samples; no sample exceeds pow(10, -1/20)
#pragma once
#include <string>
#include <vector>
namespace op::limiter {
constexpr double kLookAheadS = 0.005;  // fixed look-ahead: reduction starts this far before a peak
constexpr double kAttackS = 0.0005;    // one-pole time constant while the gain is falling
constexpr double kReleaseS = 0.050;    // one-pole time constant while the gain is rising
struct Stats {
    long long frames = 0;                  // frames processed (samples per channel)
    int sample_rate = 0;                   // sample rate used for the time constants
    int look_ahead_samples = 0;            // la = sample_rate / 200 (integer division)
    bool engaged = false;                  // true iff some frame applied a gain below 0 dB
    double max_gain_reduction_db = 0.0;    // most negative smoothed gain over all frames (<= 0)
    long long first_reduction_frame = -1;  // first frame with a smoothed gain below 0 dB, else -1
};
// Apply the brickwall peak limiter in place to interleaved float samples (docs/spec/limiter.md §Definitions).
// channels must be 1 or 2, interleaved.size() a multiple of channels, sample_rate in [8000, 192000] and
// ceiling_dbfs in [-30, 0]; otherwise returns false with an explanatory err and the buffer is untouched.
// On success stats is fully rewritten. Pure function: no wall clock, no state between calls. Usage: see header.
bool apply(std::vector<float>& interleaved, int channels, int sample_rate, double ceiling_dbfs, Stats& stats,
           std::string& err);
}  // namespace op::limiter
