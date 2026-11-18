// engine/resample.cpp — windowed-sinc resampler; see engine/resample.hpp for the API and
// docs/spec/project-model.md §"Sampler resampling — additive (WP-0138)" for the normative rules.
// Boring explicit code: per-output-sample tap loop, shared kernel values across channels,
// double accumulation throughout (spec), float32 storage.
#include "engine/resample.hpp"

#include <algorithm>
#include <cmath>

#include "engine/dsp.hpp"

namespace op::resample {

bool rate_supported(int in_rate) { return in_rate >= 8000 && in_rate <= 192000; }

bool windowsinc(const std::vector<float>& in, int channels, int in_rate, int out_rate,
                std::vector<float>& out) {
    if (channels != 1 && channels != 2) return false;
    if (in_rate <= 0 || out_rate <= 0) return false;
    const long long in_frames = static_cast<long long>(in.size()) / channels;
    // Output length = ceil(in_frames × out_rate / in_rate), exact integer arithmetic (spec).
    const long long out_frames = (in_frames * static_cast<long long>(out_rate) + in_rate - 1) / in_rate;
    out.assign(static_cast<size_t>(out_frames) * static_cast<size_t>(channels), 0.0f);
    if (in_frames == 0 || out_frames == 0) return true;

    const double scale = std::min(1.0, static_cast<double>(out_rate) / static_cast<double>(in_rate));
    const double stretch = 1.0 / scale;      // ≥ 1: kernel width multiplier when downsampling (spec)
    const double fc = 0.97 * scale;          // cutoff as fraction of input Nyquist (spec)
    const double half_w = 16.0 * stretch;    // window half-width in input samples (16 zero crossings)
    const double pi = dsp::kPi_d();

    // Kernel taps recomputed per output sample (t changes each step); buffer sized for the widest
    // possible window, reused — no allocation inside the loop.
    std::vector<double> taps(static_cast<size_t>(std::ceil(2.0 * half_w)) + 4, 0.0);

    for (long long k = 0; k < out_frames; ++k) {
        // Input position t = (k × in_rate)/out_rate: 64-bit integer part, double fraction (spec).
        const long long num = k * static_cast<long long>(in_rate);
        const long long ip = num / out_rate;
        const long long rem = num % out_rate;
        const double frac = static_cast<double>(rem) / static_cast<double>(out_rate);

        // Taps inside the window and inside the input; taps outside the input are zero (spec).
        long long lo = static_cast<long long>(std::ceil(static_cast<double>(ip) + frac - half_w));
        if (lo < 0) lo = 0;
        long long hi = static_cast<long long>(std::floor(static_cast<double>(ip) + frac + half_w));
        if (hi > in_frames - 1) hi = in_frames - 1;
        if (lo > hi) continue;  // defensive: unreachable for valid rates, keeps output zero

        double hsum = 0.0;
        size_t j = 0;
        for (long long i = lo; i <= hi; ++i, ++j) {
            const double d = static_cast<double>(ip - i) + frac;  // offset in input samples
            const double z = fc * d;
            const double sincv = (z == 0.0) ? 1.0 : dsp::sind_(pi * z) / (pi * z);
            const double u = (d + half_w) / (2.0 * half_w);  // Blackman over [−half_w, +half_w]
            const double w = 0.42 - 0.5 * dsp::cosd_(2.0 * pi * u) + 0.08 * dsp::cosd_(4.0 * pi * u);
            taps[j] = fc * sincv * w;
            hsum += taps[j];
        }
        if (!(hsum != 0.0)) continue;  // defensive: all-zero tap sum leaves this output zero

        for (int c = 0; c < channels; ++c) {
            double acc = 0.0;
            size_t jj = 0;
            for (long long i = lo; i <= hi; ++i, ++jj)
                acc += static_cast<double>(in[static_cast<size_t>(i) * static_cast<size_t>(channels) +
                                              static_cast<size_t>(c)]) * taps[jj];
            // Normalize by the sum of used taps: DC gain exactly 1 near the edges (spec).
            out[static_cast<size_t>(k) * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
                static_cast<float>(acc / hsum);
        }
    }
    return true;
}

}  // namespace op::resample
