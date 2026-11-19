// engine/analyze.cpp — objective audio features: sample peak / RMS dBFS, ITU-R BS.1770-4 integrated
// LUFS, spectral centroid. Spec: docs/spec/analyze.md (WP-0144). All arithmetic in double; sums
// accumulate sequentially in ascending index order; libm only via op::dsp wrappers (ADR-0004) — this
// file must not call std math functions directly (+ - * / and comparisons are fine).

#include "engine/analyze.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "engine/dsp.hpp"

namespace op::analyze {
namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();  // <limits>, not libm

// Non-finite samples are treated as 0.0 (spec). NaN fails every comparison; ±inf falls outside the
// finite window. Samples are spec'd to [-1,1], so the window bound is far beyond any real value.
inline double to_sample(float v) {
    if (!(v >= -1e30f && v <= 1e30f)) return 0.0;
    return static_cast<double>(v);
}

struct Biquad { double b0, b1, b2, a1, a2; };

// K-weighting stage 1 (high shelf), BS.1770-4 constants, bilinear-derived at fs. At 48 kHz these
// reproduce the ITU table values (asserted by tests/test_main.cpp to 1e-9).
Biquad k_stage1(double fs) {
    const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
    const double K = op::dsp::tand_(op::dsp::kPi_d() * f0 / fs);
    const double Vh = op::dsp::powd_(10.0, G / 20.0);
    const double Vb = op::dsp::powd_(Vh, 0.4996667741545416);
    const double KQ = K / Q, K2 = K * K;
    const double a0 = 1.0 + KQ + K2;
    Biquad c;
    c.b0 = (Vh + Vb * KQ + K2) / a0;
    c.b1 = 2.0 * (K2 - Vh) / a0;
    c.b2 = (Vh - Vb * KQ + K2) / a0;
    c.a1 = 2.0 * (K2 - 1.0) / a0;
    c.a2 = (1.0 - KQ + K2) / a0;
    return c;
}

// K-weighting stage 2 (RLB high-pass): numerator b = (1, −2, 1) EXACTLY as in the BS.1770-4 table
// (the spec's "b = (1, −2, 1)" is literal — do NOT divide by a0); only a1/a2 are normalized.
Biquad k_stage2(double fs) {
    const double f0 = 38.13547087602444, Q = 0.5003270373238773;
    const double K = op::dsp::tand_(op::dsp::kPi_d() * f0 / fs);
    const double KQ = K / Q, K2 = K * K;
    const double a0 = 1.0 + KQ + K2;
    Biquad c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (K2 - 1.0) / a0;
    c.a2 = (1.0 - KQ + K2) / a0;
    return c;
}

// Direct Form I, zero initial state: y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2].
void run_biquad(const double* x, size_t n, const Biquad& c, double* y) {
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double xn = x[i];
        const double yn = c.b0 * xn + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;
        y[i] = yn;
        x2 = x1; x1 = xn;
        y2 = y1; y1 = yn;
    }
}

// Iterative radix-2 FFT, in place (bit-reversal permutation first). Twiddles are computed directly as
// cosd_(2π·t/N), −sind_(2π·t/N) per butterfly — no recurrence (spec). N must be a power of two.
void fft_radix2(std::vector<double>& re, std::vector<double>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    const double two_pi = 2.0 * op::dsp::kPi_d();
    for (size_t len = 2; len <= n; len <<= 1) {
        const size_t half = len >> 1, step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < half; ++k) {
                const double t = static_cast<double>(k * step);
                const double wr = op::dsp::cosd_(two_pi * t / static_cast<double>(n));
                const double wi = -op::dsp::sind_(two_pi * t / static_cast<double>(n));
                const size_t a = i + k, b = i + k + half;
                const double xr = re[b] * wr - im[b] * wi;
                const double xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr; im[b] = im[a] - xi;
                re[a] += xr; im[a] += xi;
            }
        }
    }
}

// BS.1770-4 integrated loudness with absolute (−70 LUFS) and relative (Γ) gating, channel weights 1.0.
std::optional<double> integrated_lufs(const std::vector<float>& x, size_t frames, int channels, int fs) {
    if (fs <= 0 || frames == 0) return std::nullopt;
    const double fsd = static_cast<double>(fs);
    const Biquad s1 = k_stage1(fsd), s2 = k_stage2(fsd);

    // K-weight each channel over the whole file (stage 1 → stage 2 cascade, zero initial state).
    std::vector<std::vector<double>> ys;
    ys.reserve(static_cast<size_t>(channels));
    for (int c = 0; c < channels; ++c) {
        std::vector<double> buf(frames), tmp(frames);
        for (size_t i = 0; i < frames; ++i)
            buf[i] = to_sample(x[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)]);
        run_biquad(buf.data(), frames, s1, tmp.data());
        run_biquad(tmp.data(), frames, s2, buf.data());
        ys.push_back(std::move(buf));
    }

    // 400 ms blocks, 100 ms hop, complete blocks only; z_j = Σ_c (1/B)·Σ y_c².
    const long long B = (4 * static_cast<long long>(fs) + 5) / 10;
    const long long H = (static_cast<long long>(fs) + 5) / 10;
    std::vector<double> z;  // block powers in ascending block order
    for (long long j = 0; j * H + B <= static_cast<long long>(frames); ++j) {
        const size_t base = static_cast<size_t>(j * H);
        double zj = 0.0;
        for (int c = 0; c < channels; ++c) {
            const std::vector<double>& y = ys[static_cast<size_t>(c)];
            double acc = 0.0;
            for (long long i = 0; i < B; ++i) {
                const double v = y[base + static_cast<size_t>(i)];
                acc += v * v;
            }
            zj += acc / static_cast<double>(B);
        }
        z.push_back(zj);
    }
    if (z.empty()) return std::nullopt;

    auto loudness = [](double zj) { return zj == 0.0 ? kNegInf : -0.691 + 10.0 * op::dsp::log10d_(zj); };
    std::vector<double> kept;  // absolute gate: l_j > −70
    for (double zj : z) if (loudness(zj) > -70.0) kept.push_back(zj);
    if (kept.empty()) return std::nullopt;

    double mean = 0.0;  // relative gate: Γ from the mean of the kept blocks' z_j
    for (double zj : kept) mean += zj;
    mean /= static_cast<double>(kept.size());
    const double gamma = -0.691 + 10.0 * op::dsp::log10d_(mean) - 10.0;

    std::vector<double> finalz;
    for (double zj : kept) if (loudness(zj) > gamma) finalz.push_back(zj);
    if (finalz.empty()) return std::nullopt;  // unreachable: the loudest kept block always passes Γ
    double fmean = 0.0;
    for (double zj : finalz) fmean += zj;
    fmean /= static_cast<double>(finalz.size());
    return -0.691 + 10.0 * op::dsp::log10d_(fmean);
}

// Spectral centroid of the mono mix: N=2048 periodic-Hann frames, hop 1024, complete frames only;
// one global ratio Σ_frames Σ_b b·(fs/N)·|X[b]| / Σ_frames Σ_b |X[b]| over bins b = 0..N/2.
std::optional<double> spectral_centroid(const std::vector<float>& x, size_t frames, int channels, int fs) {
    constexpr size_t N = 2048, HOP = 1024;
    if (fs <= 0 || frames < N) return std::nullopt;
    const double two_pi = 2.0 * op::dsp::kPi_d();
    std::vector<double> w(N), re(N), im(N);
    for (size_t n = 0; n < N; ++n)
        w[n] = 0.5 - 0.5 * op::dsp::cosd_(two_pi * static_cast<double>(n) / static_cast<double>(N));
    const double bin_hz = static_cast<double>(fs) / static_cast<double>(N);
    double num = 0.0, den = 0.0;
    for (size_t start = 0; start + N <= frames; start += HOP) {
        for (size_t n = 0; n < N; ++n) {
            const size_t idx = start + n;
            double m;
            if (channels == 1) m = to_sample(x[idx]);
            else m = (to_sample(x[static_cast<size_t>(idx) * 2u]) + to_sample(x[static_cast<size_t>(idx) * 2u + 1u])) * 0.5;
            re[n] = m * w[n];
        }
        for (size_t n = 0; n < N; ++n) im[n] = 0.0;
        fft_radix2(re, im);
        for (size_t b = 0; b <= N / 2; ++b) {
            const double mag = op::dsp::sqrtd_(re[b] * re[b] + im[b] * im[b]);
            num += static_cast<double>(b) * bin_hz * mag;
            den += mag;
        }
    }
    if (den <= 1e-9) return std::nullopt;
    return num / den;
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

// Internal hook for the WP-0144 unit tests (deliberately NOT in analyze.hpp — the public header is
// fixed by spec): derives the K-weighting coefficients at fs into out[0..4] = stage1
// {b0,b1,b2,a1,a2}, out[5..9] = stage2 {b0,b1,b2,a1,a2}. Tests declare this extern themselves.
void k_weight_coefficients(int fs, double out[10]) {
    const Biquad s1 = k_stage1(static_cast<double>(fs)), s2 = k_stage2(static_cast<double>(fs));
    out[0] = s1.b0; out[1] = s1.b1; out[2] = s1.b2; out[3] = s1.a1; out[4] = s1.a2;
    out[5] = s2.b0; out[6] = s2.b1; out[7] = s2.b2; out[8] = s2.a1; out[9] = s2.a2;
}

Features analyze_samples(const std::vector<float>& interleaved, int channels, int sample_rate) {
    Features f;
    f.sample_rate = sample_rate;
    f.channels = channels;
    if (channels != 1 && channels != 2) return f;  // precondition enforced by the CLI; stay safe anyway
    const size_t frames = interleaved.size() / static_cast<size_t>(channels);
    f.frames = static_cast<long long>(frames);
    if (sample_rate > 0)
        f.duration_s = static_cast<double>(frames) / static_cast<double>(sample_rate);

    // Sample peaks per channel, ascending frame order; overall peak is the max of those.
    double all_max = 0.0;
    for (int c = 0; c < channels; ++c) {
        double mx = 0.0;
        for (size_t i = 0; i < frames; ++i) {
            const double v = to_sample(interleaved[static_cast<size_t>(i) * static_cast<size_t>(channels) + static_cast<size_t>(c)]);
            const double a = v < 0.0 ? -v : v;
            if (a > mx) mx = a;
        }
        if (mx > all_max) all_max = mx;
        f.peak_dbfs_per_channel.push_back((frames == 0 || mx == 0.0)
                                              ? std::optional<double>{}
                                              : std::optional<double>(20.0 * op::dsp::log10d_(mx)));
    }
    f.peak_dbfs = (frames == 0 || all_max == 0.0) ? std::optional<double>{}
                                                  : std::optional<double>(20.0 * op::dsp::log10d_(all_max));

    // RMS over every sample of every channel, ascending interleaved index order.
    double S = 0.0;
    for (size_t i = 0; i < frames * static_cast<size_t>(channels); ++i) {
        const double v = to_sample(interleaved[i]);
        S += v * v;
    }
    f.rms_dbfs = (frames == 0 || S == 0.0)
                     ? std::optional<double>{}
                     : std::optional<double>(20.0 * op::dsp::log10d_(
                           op::dsp::sqrtd_(S / static_cast<double>(frames * static_cast<size_t>(channels)))));

    f.integrated_lufs = integrated_lufs(interleaved, frames, channels, sample_rate);
    f.spectral_centroid_hz = spectral_centroid(interleaved, frames, channels, sample_rate);
    return f;
}

std::string to_json(const Features& f) {
    char buf[64];
    std::string out;
    out += "{\n";
    std::snprintf(buf, sizeof buf, "  \"sample_rate\": %d,\n", f.sample_rate); out += buf;
    std::snprintf(buf, sizeof buf, "  \"channels\": %d,\n", f.channels); out += buf;
    std::snprintf(buf, sizeof buf, "  \"frames\": %lld,\n", f.frames); out += buf;
    std::snprintf(buf, sizeof buf, "  \"duration_s\": %.6f,\n", f.duration_s); out += buf;
    out += "  \"peak_dbfs\": " + fmt_opt(f.peak_dbfs) + ",\n";
    out += "  \"peak_dbfs_per_channel\": [";
    for (size_t c = 0; c < f.peak_dbfs_per_channel.size(); ++c) {
        if (c > 0) out += ", ";
        out += fmt_opt(f.peak_dbfs_per_channel[c]);
    }
    out += "],\n";
    out += "  \"rms_dbfs\": " + fmt_opt(f.rms_dbfs) + ",\n";
    out += "  \"integrated_lufs\": " + fmt_opt(f.integrated_lufs) + ",\n";
    out += "  \"spectral_centroid_hz\": " + fmt_opt(f.spectral_centroid_hz) + "\n";
    out += "}\n";
    return out;
}

}  // namespace op::analyze
