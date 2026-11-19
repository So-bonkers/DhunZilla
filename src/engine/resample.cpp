// engine/resample.cpp — windowed-sinc resampler; see engine/resample.hpp for the API and
// docs/spec/project-model.md §"Sampler resampling — additive (WP-0138)" plus
// §"Polyphase tap table — additive (WP-0150)" for the normative rules.
// Boring explicit code: per-output-sample tap loop, shared kernel values across channels,
// double accumulation throughout (spec), float32 storage.
// WP-0150: taps come from a lazily-filled polyphase table (rem is always a multiple of
// gcd(in_rate, out_rate), so only out_rate/g distinct phases exist). The tap expressions and all
// summation orders are identical to the direct method below, which stays as the fallback — output
// is bit-identical for every rate pair.
#include "engine/resample.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/dsp.hpp"

namespace op::resample {

bool rate_supported(int in_rate) { return in_rate >= 8000 && in_rate <= 192000; }

namespace {

// Memory bound for the polyphase table (spec WP-0150): at most 2^20 doubles (8 MiB); above that
// the direct method is used instead (same bits).
constexpr long long kMaxTableDoubles = 1LL << 20;

// Normative reference and fallback: the pre-WP-0150 body of windowsinc, unchanged. Every tap's
// sin/cos recomputed per output sample.
bool windowsinc_direct(const std::vector<float>& in, int channels, int in_rate, int out_rate,
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

}  // namespace

bool windowsinc(const std::vector<float>& in, int channels, int in_rate, int out_rate,
                std::vector<float>& out) {
    if (channels != 1 && channels != 2) return false;
    if (in_rate <= 0 || out_rate <= 0) return false;
    const long long g = static_cast<long long>(std::gcd(in_rate, out_rate));
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

    // rem = (k × in_rate) mod out_rate is always a multiple of g, so only out_rate/g distinct
    // fractions exist (spec WP-0150). Each row holds the taps for input offsets m = −R … +R.
    const long long R = static_cast<long long>(std::ceil(half_w)) + 2;
    const long long W = 2 * R + 1;
    const long long phases = out_rate / g;
    if (phases > kMaxTableDoubles / W) return windowsinc_direct(in, channels, in_rate, out_rate, out);

    std::vector<double> table(static_cast<size_t>(phases) * static_cast<size_t>(W), 0.0);
    std::vector<char> filled(static_cast<size_t>(phases), 0);

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

        // Defensive: a window wider than the table cannot be served from it (unreachable for
        // valid rates); hand the whole call to the direct method (same bits).
        if (ip - lo > R || ip - hi < -R) return windowsinc_direct(in, channels, in_rate, out_rate, out);

        const long long ph = rem / g;
        const size_t row = static_cast<size_t>(ph) * static_cast<size_t>(W);
        if (!filled[static_cast<size_t>(ph)]) {
            // Fill lazily with the SAME expressions and operation order as the direct method.
            // frac here equals every frac that will index this row (rem = ph × g exactly).
            for (long long m = -R; m <= R; ++m) {
                const double d = static_cast<double>(m) + frac;  // offset in input samples
                const double z = fc * d;
                const double sincv = (z == 0.0) ? 1.0 : dsp::sind_(pi * z) / (pi * z);
                const double u = (d + half_w) / (2.0 * half_w);  // Blackman over [−half_w, +half_w]
                const double w = 0.42 - 0.5 * dsp::cosd_(2.0 * pi * u) + 0.08 * dsp::cosd_(4.0 * pi * u);
                table[row + static_cast<size_t>(m + R)] = fc * sincv * w;
            }
            filled[static_cast<size_t>(ph)] = 1;
        }

        double hsum = 0.0;
        for (long long i = lo; i <= hi; ++i)
            hsum += table[row + static_cast<size_t>((ip - i) + R)];
        if (!(hsum != 0.0)) continue;  // defensive: all-zero tap sum leaves this output zero

        for (int c = 0; c < channels; ++c) {
            double acc = 0.0;
            for (long long i = lo; i <= hi; ++i)
                acc += static_cast<double>(in[static_cast<size_t>(i) * static_cast<size_t>(channels) +
                                              static_cast<size_t>(c)]) *
                       table[row + static_cast<size_t>((ip - i) + R)];
            // Normalize by the sum of used taps: DC gain exactly 1 near the edges (spec).
            out[static_cast<size_t>(k) * static_cast<size_t>(channels) + static_cast<size_t>(c)] =
                static_cast<float>(acc / hsum);
        }
    }
    return true;
}

}  // namespace op::resample
