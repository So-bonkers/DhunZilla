// engine/resample.hpp — deterministic windowed-sinc resampler (WP-0138).
// Normative spec: docs/spec/project-model.md §"Sampler resampling — additive (WP-0138)".
// Used once per sampler asset at load time; bit-exact across CI arches (ADR-0004/0006).
#pragma once
#include <vector>

namespace op::resample {

// Supported *input* sample rates for the resampler: 8000–192000 Hz inclusive.
// Example: if (!op::resample::rate_supported(rr.sample_rate)) { /* render error */ }
bool rate_supported(int in_rate);

// Resample interleaved float audio from in_rate to out_rate with a fixed windowed-sinc filter:
// half-width 16 zero crossings of the input sinc per side (stretched by 1/min(1, out/in) when
// downsampling), Blackman window over that span, cutoff fc = 0.97 × min(1, out_rate/in_rate) as a
// fraction of input Nyquist. Output sample k sits at input position t = (k × in_rate)/out_rate
// (64-bit integer part + double fraction — no cumulative drift); output length is exactly
// ceil(in_frames × out_rate / in_rate). Taps outside the input are zero; each output is normalized
// by the sum of the taps used (DC gain exactly 1 at the edges). Double accumulation, float result.
// Returns false only on degenerate arguments (channels not 1/2, non-positive rates); rate-policy
// checks belong to the caller. in.size() must be a multiple of channels.
// Example:
//   std::vector<float> out;
//   op::resample::windowsinc(kit.samples, kit.channels, kit.sample_rate /*44100*/, 48000, out);
bool windowsinc(const std::vector<float>& in, int channels, int in_rate, int out_rate,
                std::vector<float>& out);

}  // namespace op::resample
