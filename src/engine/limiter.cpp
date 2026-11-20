// engine/limiter.cpp — see limiter.hpp / docs/spec/limiter.md (WP-0176).
// All arithmetic in double, libm only through op::dsp::powd_ / op::dsp::log10d_ (ADR-0004).
// Pure function of its arguments: no wall clock, no state between calls.
#include "engine/limiter.hpp"

#include "engine/dsp.hpp"

namespace op::limiter {

// A sample not in [-1e30f, 1e30f] (NaN, +-inf or a huge magnitude) is replaced by 0.0f before use — both when
// measuring and when applying the gain (spec §Definitions). The negated range test is false for NaN because
// every comparison with NaN is false; +-inf and |x| > 1e30 are caught by the bounds themselves.
static float sanitize(float x) {
    if (!(x >= -1e30f && x <= 1e30f)) return 0.0f;
    return x;
}

bool apply(std::vector<float>& interleaved, int channels, int sample_rate, double ceiling_dbfs, Stats& stats,
           std::string& err) {
    // Validation order is normative (spec §API); on failure the buffer and stats are left untouched.
    if (channels != 1 && channels != 2) {
        err = "limiter: channels must be 1 or 2";
        return false;
    }
    if (interleaved.size() % static_cast<size_t>(channels) != 0) {
        err = "limiter: sample count not a multiple of channels";
        return false;
    }
    if (sample_rate < 8000 || sample_rate > 192000) {
        err = "limiter: unsupported sample rate " + std::to_string(sample_rate);
        return false;
    }
    if (!(ceiling_dbfs >= -30.0 && ceiling_dbfs <= 0.0)) {  // negated AND: NaN is out of range too
        err = "limiter: ceiling out of range [-30, 0]";
        return false;
    }

    const long long frames = static_cast<long long>(interleaved.size()) / channels;
    const int la = sample_rate / 200;                       // fixed look-ahead, integer division (spec)
    const double dt = 1.0 / static_cast<double>(sample_rate);
    const double a_attack = dt / (kAttackS + dt);           // backward-Euler one-pole, falling gain
    const double a_release = dt / (kReleaseS + dt);         // backward-Euler one-pole, rising gain
    const double ceiling_lin = op::dsp::powd_(10.0, ceiling_dbfs / 20.0);
    const float cl = static_cast<float>(ceiling_lin);

    bool engaged = false;
    double min_g = 0.0;                 // most negative smoothed gain over all frames (<= 0)
    long long first_reduction = -1;     // first frame with a smoothed gain below 0 dB, else -1
    double g = 0.0;                     // one smoothed-gain state shared by all channels

    for (long long n = 0; n < frames; ++n) {
        // 1 + 2: stereo-linked look-ahead peak p[n] = max(m[n .. min(n + la - 1, F - 1)]), m = |x| per frame.
        // A straightforward scan of up to la frames (spec): every read index is >= n and the output for frame n
        // is written only after its own scan, so the in-place update never feeds back into a look-ahead window.
        const long long hi = n + la < frames ? n + la : frames;  // exclusive end; always > n since la >= 40
        double p = 0.0;                                          // magnitudes are >= 0, so 0.0 is a lower bound
        for (long long j = n; j < hi; ++j) {
            const size_t base = static_cast<size_t>(j) * static_cast<size_t>(channels);
            for (int c = 0; c < channels; ++c) {
                const float v = sanitize(interleaved[base + static_cast<size_t>(c)]);
                const double mag = v < 0.0f ? -static_cast<double>(v) : static_cast<double>(v);
                if (mag > p) p = mag;
            }
        }

        // 3: desired gain in dB — strict comparison, material exactly at the ceiling is not over it.
        const double d = p > ceiling_lin ? ceiling_dbfs - 20.0 * op::dsp::log10d_(p) : 0.0;

        // 4: smoothed gain (one-pole attack while falling, release while rising).
        if (d < g) g = g + a_attack * (d - g);
        else g = g + a_release * (d - g);
        if (g < 0.0) {
            engaged = true;
            if (first_reduction < 0) first_reduction = n;
        }
        if (g < min_g) min_g = g;

        // 5: apply the smoothed gain and hard-clamp to +-ceiling on the float samples.
        const double gl = op::dsp::powd_(10.0, g / 20.0);
        const size_t base = static_cast<size_t>(n) * static_cast<size_t>(channels);
        for (int c = 0; c < channels; ++c) {
            const float x = sanitize(interleaved[base + static_cast<size_t>(c)]);
            float y = static_cast<float>(static_cast<double>(x) * gl);
            if (y > cl) y = cl;
            else if (y < -cl) y = -cl;
            interleaved[base + static_cast<size_t>(c)] = y;
        }
    }

    stats.frames = frames;
    stats.sample_rate = sample_rate;
    stats.look_ahead_samples = la;
    stats.engaged = engaged;
    stats.max_gain_reduction_db = min_g;
    stats.first_reduction_frame = first_reduction;
    return true;
}

}  // namespace op::limiter
