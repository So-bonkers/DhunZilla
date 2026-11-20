// engine/loudness.cpp — true peak (dBTP), momentary / short-term maximum LUFS, EBU Tech 3342 loudness
// range. Spec: docs/spec/loudness.md (WP-0168). All arithmetic in double; sums accumulate sequentially
// in ascending index order; libm only via op::dsp wrappers (ADR-0004) — this file must not call std math
// functions directly (+ - * / , comparisons and std::sort are fine).

#include "engine/loudness.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/dsp.hpp"

namespace op::analyze {
// Test/internal hook already defined in analyze.cpp (WP-0144): K-weighting coefficients for fs,
// out[0..4] = stage 1 {b0,b1,b2,a1,a2}, out[5..9] = stage 2. Declared here so the two modules share
// exactly one definition of the filter (docs/spec/loudness.md §Definitions).
void k_weight_coefficients(int fs, double out[10]);
}  // namespace op::analyze

namespace op::loudness {
namespace {

constexpr int kTaps = 24;      // true-peak filter taps, k = -11 .. +12 (spec)
constexpr int kTapLo = -11;    // first tap index
constexpr int kTapHi = 12;     // last tap index
constexpr double kWinHalf = 12.0;  // raised-cosine window half-width of the interpolation filter

// Non-finite samples are treated as 0.0 (spec). NaN fails every comparison; ±inf falls outside the
// finite window. Samples are spec'd to [-1,1], so the window bound is far beyond any real value.
inline double to_sample(float v) {
    if (!(v >= -1e30f && v <= 1e30f)) return 0.0;
    return static_cast<double>(v);
}

// Sample x[j] of channel c, or 0.0 outside [0, frames) (spec: zero padding for the interpolator).
inline double sample_at(const float* x, long long frames, int channels, int c, long long j) {
    if (j < 0 || j >= frames) return 0.0;
    return to_sample(x[static_cast<size_t>(j) * static_cast<size_t>(channels) + static_cast<size_t>(c)]);
}

// Interpolation kernel h(t) = sinc(t)·(0.5 + 0.5·cos(π·t/12)) for |t| < 12, else 0 (spec).
double interp_kernel(double t) {
    if (!(t > -kWinHalf && t < kWinHalf)) return 0.0;
    const double pi = op::dsp::kPi_d();
    const double pt = pi * t;
    const double sinc = (t == 0.0) ? 1.0 : op::dsp::sind_(pt) / pt;
    return sinc * (0.5 + 0.5 * op::dsp::cosd_(pi * t / kWinHalf));
}

// taps[ph - 1][k - kTapLo] = h(k - ph/4): the three fractional-delay kernels for the interpolated
// positions 1/4, 2/4 and 3/4 between sample i and i+1. Computed once; identical to evaluating
// interp_kernel() per sample because the argument is the same value.
void build_taps(double taps[3][kTaps]) {
    for (int ph = 1; ph <= 3; ++ph) {
        const double off = static_cast<double>(ph) / 4.0;
        for (int k = kTapLo; k <= kTapHi; ++k)
            taps[ph - 1][k - kTapLo] = interp_kernel(static_cast<double>(k) - off);
    }
}

// Largest |value| over the original samples and all interpolated samples of every channel.
double true_peak_magnitude(const std::vector<float>& x, long long frames, int channels) {
    double taps[3][kTaps];
    build_taps(taps);
    const float* p = x.data();
    double mx = 0.0;
    for (int c = 0; c < channels; ++c) {
        for (long long i = 0; i < frames; ++i) {
            const double v = sample_at(p, frames, channels, c, i);
            const double a = v < 0.0 ? -v : v;
            if (a > mx) mx = a;
        }
        for (long long i = 0; i + 1 < frames; ++i) {
            for (int ph = 1; ph <= 3; ++ph) {
                double y = 0.0;
                for (int k = kTapLo; k <= kTapHi; ++k)
                    y += sample_at(p, frames, channels, c, i + k) * taps[ph - 1][k - kTapLo];
                const double a = y < 0.0 ? -y : y;
                if (a > mx) mx = a;
            }
        }
    }
    return mx;
}

// K-weighted per-frame power p[i] = Σ_c y_c[i]², weights 1.0, with the same two-biquad cascade and
// Direct Form I zero-state filtering as `opd analyze` (coefficients shared via k_weight_coefficients).
void k_weighted_power(const std::vector<float>& x, long long frames, int channels, int fs,
                      std::vector<double>& out_p) {
    double coef[10];
    op::analyze::k_weight_coefficients(fs, coef);
    out_p.assign(static_cast<size_t>(frames), 0.0);
    std::vector<double> buf(static_cast<size_t>(frames)), tmp(static_cast<size_t>(frames));
    for (int c = 0; c < channels; ++c) {
        for (long long i = 0; i < frames; ++i)
            buf[static_cast<size_t>(i)] = sample_at(x.data(), frames, channels, c, i);
        // stage 1: y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2], zero initial state
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        for (long long i = 0; i < frames; ++i) {
            const double xn = buf[static_cast<size_t>(i)];
            const double yn = coef[0] * xn + coef[1] * x1 + coef[2] * x2 - coef[3] * y1 - coef[4] * y2;
            tmp[static_cast<size_t>(i)] = yn;
            x2 = x1; x1 = xn;
            y2 = y1; y1 = yn;
        }
        // stage 2
        x1 = x2 = y1 = y2 = 0.0;
        for (long long i = 0; i < frames; ++i) {
            const double xn = tmp[static_cast<size_t>(i)];
            const double yn = coef[5] * xn + coef[6] * x1 + coef[7] * x2 - coef[8] * y1 - coef[9] * y2;
            buf[static_cast<size_t>(i)] = yn;
            x2 = x1; x1 = xn;
            y2 = y1; y1 = yn;
        }
        for (long long i = 0; i < frames; ++i) {
            const double v = buf[static_cast<size_t>(i)];
            out_p[static_cast<size_t>(i)] += v * v;
        }
    }
}

// Prefix sums P[0] = 0, P[i+1] = P[i] + p[i], so the mean power of [s, s+len) is (P[s+len] − P[s])/len.
void prefix_sums(const std::vector<double>& p, std::vector<double>& P) {
    P.assign(p.size() + 1u, 0.0);
    for (size_t i = 0; i < p.size(); ++i) P[i + 1u] = P[i] + p[i];
}

// Mean power of every complete window [s, s+len) with s = 0, step, 2·step, ... in ascending order.
void window_powers(const std::vector<double>& P, long long frames, long long len, long long step,
                   std::vector<double>& z) {
    z.clear();
    for (long long s = 0; s + len <= frames; s += step) {
        const double acc = P[static_cast<size_t>(s + len)] - P[static_cast<size_t>(s)];
        z.push_back(acc / static_cast<double>(len));
    }
}

// L(z) = −0.691 + 10·log10(z) for z > 0 (the callers never feed z <= 0 to this).
inline double loudness_of(double z) { return -0.691 + 10.0 * op::dsp::log10d_(z); }

// Max of the window powers; nullopt when there is no complete window or the maximum is 0 (spec).
std::optional<double> max_loudness(const std::vector<double>& z) {
    if (z.empty()) return std::nullopt;
    double mx = 0.0;
    for (double v : z)
        if (v > mx) mx = v;
    if (mx == 0.0) return std::nullopt;
    return loudness_of(mx);
}

// EBU Tech 3342 loudness range over the given window powers: absolute gate −70 LUFS, relative gate
// Γ = L(mean of kept z) − 20, then LRA = P95 − P10 with linear interpolation between neighbours.
std::optional<double> loudness_range(const std::vector<double>& z) {
    std::vector<double> kept;
    for (double v : z)
        if (v > 0.0 && loudness_of(v) > -70.0) kept.push_back(v);
    if (kept.empty()) return std::nullopt;

    double mean = 0.0;
    for (double v : kept) mean += v;
    mean /= static_cast<double>(kept.size());
    const double gate = loudness_of(mean) - 20.0;

    std::vector<double> l;
    for (double v : kept) {
        const double lv = loudness_of(v);
        if (lv > gate) l.push_back(lv);
    }
    if (l.empty()) return std::nullopt;  // unreachable: the loudest kept window always passes Γ

    std::sort(l.begin(), l.end());
    const size_t n = l.size();
    auto pct = [n, &l](double q) {
        const double pos = q * static_cast<double>(n - 1u);
        const size_t lo = static_cast<size_t>(pos);
        const double frac = pos - static_cast<double>(lo);
        if (lo + 1u >= n) return l[lo];
        return l[lo] + frac * (l[lo + 1u] - l[lo]);
    };
    return pct(0.95) - pct(0.10);
}

// %.4f with the spec's negative-zero rule: "-0.0000" is printed as "0.0000".
std::string fmt4(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    if (std::strcmp(buf, "-0.0000") == 0) return "0.0000";
    return std::string(buf);
}

std::string fmt_opt(const std::optional<double>& v) { return v ? fmt4(*v) : std::string("null"); }

}  // namespace

Result measure(const std::vector<float>& interleaved, int channels, int sample_rate) {
    Result r;
    r.sample_rate = sample_rate;
    r.channels = channels;
    if (channels != 1 && channels != 2) return r;  // precondition enforced by the CLI; stay safe anyway
    const long long frames =
        static_cast<long long>(interleaved.size() / static_cast<size_t>(channels));
    r.frames = frames;
    if (sample_rate > 0)
        r.duration_s = static_cast<double>(frames) / static_cast<double>(sample_rate);
    if (frames == 0 || sample_rate <= 0) return r;  // every optional stays empty (spec)

    const double tp = true_peak_magnitude(interleaved, frames, channels);
    if (tp > 0.0) r.true_peak_dbtp = 20.0 * op::dsp::log10d_(tp);

    std::vector<double> p, P;
    k_weighted_power(interleaved, frames, channels, sample_rate, p);
    prefix_sums(p, P);

    const long long fs = static_cast<long long>(sample_rate);
    const long long hop = (fs + 5) / 10;

    std::vector<double> z;
    window_powers(P, frames, (4 * fs + 5) / 10, hop, z);
    r.momentary_max_lufs = max_loudness(z);

    window_powers(P, frames, 3 * fs, hop, z);
    r.short_term_max_lufs = max_loudness(z);

    window_powers(P, frames, 3 * fs, fs, z);
    r.loudness_range_lu = loudness_range(z);
    return r;
}

std::string to_json(const Result& r) {
    char buf[64];
    std::string out;
    out += "{\n";
    std::snprintf(buf, sizeof buf, "  \"sample_rate\": %d,\n", r.sample_rate); out += buf;
    std::snprintf(buf, sizeof buf, "  \"channels\": %d,\n", r.channels); out += buf;
    std::snprintf(buf, sizeof buf, "  \"frames\": %lld,\n", r.frames); out += buf;
    std::snprintf(buf, sizeof buf, "  \"duration_s\": %.6f,\n", r.duration_s); out += buf;
    out += "  \"true_peak_dbtp\": " + fmt_opt(r.true_peak_dbtp) + ",\n";
    out += "  \"momentary_max_lufs\": " + fmt_opt(r.momentary_max_lufs) + ",\n";
    out += "  \"short_term_max_lufs\": " + fmt_opt(r.short_term_max_lufs) + ",\n";
    out += "  \"loudness_range_lu\": " + fmt_opt(r.loudness_range_lu) + "\n";
    out += "}\n";
    return out;
}

}  // namespace op::loudness
