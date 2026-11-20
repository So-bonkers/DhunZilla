// Unit tests for op::limiter (WP-0176, docs/spec/limiter.md). Boring assert-based runner — no framework deps.
// No filesystem access at all (no temp dirs). Exit code = number of failures. Run: ./build/op_limiter_tests
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engine/limiter.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

static const double kTwoPi = 6.28318530717958647692528676655900577;

// Bit-for-bit comparison of two float buffers (what "bit-identical" means in the spec).
static bool same_bytes(const std::vector<float>& a, const std::vector<float>& b) {
    return a.size() == b.size() &&
           std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// The four normative validation messages, in order, and that a rejected call leaves the buffer untouched.
static void test_validation_messages() {
    const std::vector<float> good = {0.5f, -0.5f, 0.25f, 0.75f};
    const float nan_v = std::nanf("");
    op::limiter::Stats st;
    std::string err;

    std::vector<float> buf = good;
    CHECK(!op::limiter::apply(buf, 3, 48000, -1.0, st, err) && err == "limiter: channels must be 1 or 2",
          "validate.channels_3_message");
    buf = good;
    CHECK(!op::limiter::apply(buf, 0, 48000, -1.0, st, err) && err == "limiter: channels must be 1 or 2",
          "validate.channels_0_message");
    buf = {0.5f, -0.5f, 0.25f};  // three samples with channels == 2
    CHECK(!op::limiter::apply(buf, 2, 48000, -1.0, st, err) &&
              err == "limiter: sample count not a multiple of channels",
          "validate.sample_count_message");
    buf = good;
    CHECK(!op::limiter::apply(buf, 2, 4000, -1.0, st, err) &&
              err == "limiter: unsupported sample rate 4000",
          "validate.sample_rate_low_message");
    buf = good;
    CHECK(!op::limiter::apply(buf, 2, 192001, -1.0, st, err) &&
              err == "limiter: unsupported sample rate 192001",
          "validate.sample_rate_high_message");
    buf = good;
    CHECK(!op::limiter::apply(buf, 2, 48000, 0.5, st, err) &&
              err == "limiter: ceiling out of range [-30, 0]",
          "validate.ceiling_above_message");
    buf = good;
    CHECK(!op::limiter::apply(buf, 2, 48000, -30.5, st, err) &&
              err == "limiter: ceiling out of range [-30, 0]",
          "validate.ceiling_below_message");
    buf = good;  // NaN fails every comparison, so it must be rejected too
    CHECK(!op::limiter::apply(buf, 2, 48000, nan_v, st, err) &&
              err == "limiter: ceiling out of range [-30, 0]",
          "validate.ceiling_nan_message");

    // Every rejected call leaves the buffer untouched, bit for bit.
    bool untouched = true;
    const int bad_channels[] = {0, 3};
    for (const int ch : bad_channels) {
        std::vector<float> b = good;
        if (op::limiter::apply(b, ch, 48000, -1.0, st, err)) untouched = false;
        if (!same_bytes(b, good)) untouched = false;
    }
    {
        std::vector<float> b = {0.5f, -0.5f, 0.25f};
        const std::vector<float> want = b;
        if (op::limiter::apply(b, 2, 48000, -1.0, st, err)) untouched = false;
        if (!same_bytes(b, want)) untouched = false;
    }
    for (const int sr : {4000, 192001}) {
        std::vector<float> b = good;
        if (op::limiter::apply(b, 2, sr, -1.0, st, err)) untouched = false;
        if (!same_bytes(b, good)) untouched = false;
    }
    const double bad_ceilings[] = {0.5, -30.5, nan_v, INFINITY, -INFINITY};
    for (const double c : bad_ceilings) {
        std::vector<float> b = good;
        if (op::limiter::apply(b, 2, 48000, c, st, err)) untouched = false;
        if (!same_bytes(b, good)) untouched = false;
    }
    CHECK(untouched, "validate.buffer_untouched");

    // Inclusive boundaries are accepted: rates 8000 and 192000, ceilings -30.0 and 0.0 (silence -> idle).
    bool boundaries_ok = true;
    const int ok_rates[] = {8000, 192000};
    const double ok_ceilings[] = {-30.0, 0.0};
    for (const int sr : ok_rates) {
        for (const double c : ok_ceilings) {
            std::vector<float> b(64 * 2, 0.0f);
            op::limiter::Stats s;
            std::string e;
            if (!op::limiter::apply(b, 2, sr, c, s, e)) boundaries_ok = false;
            if (!e.empty()) boundaries_ok = false;
        }
    }
    CHECK(boundaries_ok, "validate.boundaries_accepted");
}

// Zero frames is a success with an idle Stats — and stats are fully rewritten, never left stale.
static void test_zero_frames_idle_stats() {
    std::vector<float> buf;  // empty
    op::limiter::Stats st;
    st.frames = 12345;  // deliberately stale on entry
    st.sample_rate = 7;
    st.look_ahead_samples = 99;
    st.engaged = true;
    st.max_gain_reduction_db = -42.0;
    st.first_reduction_frame = 7;
    std::string err;
    const bool ok = op::limiter::apply(buf, 2, 48000, -6.0, st, err);
    CHECK(ok && err.empty() && st.frames == 0 && st.sample_rate == 48000 && st.look_ahead_samples == 240 &&
              !st.engaged && st.max_gain_reduction_db == 0.0 && st.first_reduction_frame == -1,
          "zero_frames.idle_stats_rewritten");
}

// la = sample_rate / 200 (integer division) for the three rates the spec names.
static int la_at(int sample_rate) {
    std::vector<float> buf(static_cast<size_t>(8) * 2u, 0.0f);
    op::limiter::Stats st;
    std::string err;
    if (!op::limiter::apply(buf, 2, sample_rate, -6.0, st, err)) return -1;
    return st.look_ahead_samples;
}

static void test_look_ahead_samples() {
    CHECK(la_at(48000) == 240, "la.48000_is_240");
    CHECK(la_at(44100) == 220, "la.44100_is_220");  // 44100 / 200 = 220 (integer division)
    CHECK(la_at(8000) == 40, "la.8000_is_40");
}

// Material below the ceiling: every output sample bit-identical to the input, engaged == false.
static void test_below_ceiling_bit_identical() {
    const int sr = 48000;
    std::vector<float> buf;
    buf.reserve(static_cast<size_t>(sr) * 2u);
    for (int n = 0; n < sr; ++n) {  // 100 Hz sine at 0.3 amplitude, L and inverted R — peak far under -6 dBFS
        const float v = static_cast<float>(0.3 * std::sin(kTwoPi * 100.0 * static_cast<double>(n) / sr));
        buf.push_back(v);
        buf.push_back(-v);
    }
    const std::vector<float> in = buf;
    op::limiter::Stats st;
    std::string err;
    const bool ok = op::limiter::apply(buf, 2, sr, -6.0, st, err);
    CHECK(ok && same_bytes(buf, in), "below_ceiling.bit_identical");
    CHECK(!st.engaged && st.first_reduction_frame == -1 && st.max_gain_reduction_db == 0.0 && st.frames == sr,
          "below_ceiling.idle_stats");
}

// A full-scale impulse in silence: reduction starts exactly la-1 frames early, nothing exceeds the ceiling and
// the impulse lands on it (within 1 %).
static void test_impulse_in_silence() {
    const int sr = 48000;
    const int la = 240;
    const long long k = 1000;
    const double ceiling_db = -6.0;
    const float cl = static_cast<float>(std::pow(10.0, ceiling_db / 20.0));  // ~0.50118724f (tests may use libm)

    std::vector<float> buf(static_cast<size_t>(3 * sr), 0.0f);  // mono, 3 s of silence
    buf[static_cast<size_t>(k)] = 1.0f;                          // full-scale impulse at frame k
    op::limiter::Stats st;
    std::string err;
    const bool ok = op::limiter::apply(buf, 1, sr, ceiling_db, st, err);

    CHECK(ok && st.engaged && st.first_reduction_frame == k - la + 1, "impulse.first_reduction_frame");

    bool within_ceiling = true;
    for (const float s : buf) {
        if (s > cl || !(s >= -cl)) within_ceiling = false;  // second test also catches NaN
    }
    CHECK(within_ceiling, "impulse.no_sample_above_ceiling");

    const float yk = buf[static_cast<size_t>(k)];
    CHECK(yk > 0.99f * cl && yk <= cl, "impulse.hits_ceiling_within_1pct");

    // 240 attack steps at a_attack = 1/25 (48 kHz) put g within 0.01 dB of the ceiling gain.
    CHECK(st.max_gain_reduction_db > ceiling_db - 0.01 && st.max_gain_reduction_db < ceiling_db + 0.01,
          "impulse.max_reduction_within_0p01_db_of_ceiling");

    // Silence before the look-ahead window is untouched, bit for bit (g still exactly 0 dB there).
    CHECK(buf[static_cast<size_t>(k - la - 1)] == 0.0f && buf[static_cast<size_t>(k + 2 * sr)] == 0.0f,
          "impulse.silence_stays_zero");
}

// Stereo linking: a loud left transient attenuates the quiet right channel near it — and not ~1 s later.
static void test_stereo_link() {
    const int sr = 48000;
    const long long k = 4800;
    std::vector<float> buf(static_cast<size_t>(2 * sr) * 2u, 0.0f);  // 2 s stereo silence
    for (long long n = 0; n < 2 * sr; ++n) buf[static_cast<size_t>(n) * 2u + 1u] = 0.2f;  // quiet right pad
    buf[static_cast<size_t>(k) * 2u] = 1.0f;  // loud left transient only — the right channel never exceeds -6 dB

    const std::vector<float> in = buf;
    op::limiter::Stats st;
    std::string err;
    const bool ok = op::limiter::apply(buf, 2, sr, -6.0, st, err);
    CHECK(ok && st.engaged, "stereo_link.ok_and_engaged");

    // Near the hit the right channel is pulled down hard (shared gain ~-6 dB), although its own peak is 0.2.
    const float r_hit = buf[static_cast<size_t>(k) * 2u + 1u];
    CHECK(r_hit < in[static_cast<size_t>(k) * 2u + 1u] * 0.6f && r_hit > 0.0f,
          "stereo_link.right_attenuated_at_hit");

    // ~1 s later the release (tau = 50 ms) has recovered: back to the pad value within 1e-3.
    const long long m = k + sr;
    const float r_late = buf[static_cast<size_t>(m) * 2u + 1u];
    CHECK(std::fabs(r_late - in[static_cast<size_t>(m) * 2u + 1u]) <= 0.001f,
          "stereo_link.right_recovered_1s_later");

    // Before the look-ahead window nothing has happened yet: bit-exact.
    const long long b = k - 300;
    CHECK(buf[static_cast<size_t>(b) * 2u + 1u] == in[static_cast<size_t>(b) * 2u + 1u],
          "stereo_link.right_untouched_before_lookahead");
}

// A steady sine above the ceiling settles at ceiling_dbfs - 20*log10(A) (within 0.05 dB).
static void test_sine_settles_at_target_gain() {
    const int sr = 48000;
    const double ceiling_db = -6.0;
    const double A = 0.9;
    const long long F = 2 * static_cast<long long>(sr);  // 2 s, plenty for the attack to settle
    std::vector<float> buf;
    buf.reserve(static_cast<size_t>(F));
    for (long long n = 0; n < F; ++n) {  // 220 Hz: a peak inside every 5 ms look-ahead window
        buf.push_back(static_cast<float>(A * std::sin(kTwoPi * 220.0 * static_cast<double>(n) / sr)));
    }
    op::limiter::Stats st;
    std::string err;
    const bool ok = op::limiter::apply(buf, 1, sr, ceiling_db, st, err);

    const double want = ceiling_db - 20.0 * std::log10(A);  // ~-5.0849 dB
    CHECK(ok && std::fabs(st.max_gain_reduction_db - want) <= 0.05, "sine.settles_within_0p05_db");
    CHECK(st.first_reduction_frame == 0, "sine.reduces_from_frame_0");

    const float cl = static_cast<float>(std::pow(10.0, ceiling_db / 20.0));
    bool within_ceiling = true;
    for (const float s : buf) {
        if (s > cl || !(s >= -cl)) within_ceiling = false;
    }
    CHECK(within_ceiling, "sine.output_within_ceiling");
}

// Attack vs release: still clearly attenuated 5 ms after a hit, back at the pad value within 0.001 after ~1 s.
static void test_attack_vs_release() {
    const int sr = 48000;
    const int la = 240;
    const long long k = 4800;
    const float pad = 0.3f;
    std::vector<float> buf(static_cast<size_t>(2 * sr), pad);  // mono, 2 s constant pad
    buf[static_cast<size_t>(k)] = 1.0f;                         // hit above the -6 dBFS ceiling
    op::limiter::Stats st;
    std::string err;
    const bool ok = op::limiter::apply(buf, 1, sr, -6.0, st, err);

    // 5 ms after the hit: the release (tau = 50 ms) has barely begun — still clearly attenuated.
    CHECK(ok && buf[static_cast<size_t>(k + la)] < pad * 0.7f && buf[static_cast<size_t>(k + la)] > 0.0f,
          "attack_release.still_attenuated_5ms_after");

    // ~1 s after the hit: gain is back at unity, so the pad is back to within 0.001 of its input value.
    CHECK(std::fabs(buf[static_cast<size_t>(k + sr)] - pad) <= 0.001f, "attack_release.pad_back_after_1s");

    // And the hit itself sits on the ceiling (never above it).
    const float cl = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    CHECK(buf[static_cast<size_t>(k)] <= cl && buf[static_cast<size_t>(k)] > 0.99f * cl,
          "attack_release.hit_on_ceiling");
}

// Two identical inputs give bit-identical output and identical stats (no hidden state between calls).
static void test_determinism() {
    std::vector<float> a;
    a.reserve(4000);
    unsigned long long s = 123456789ULL;  // fixed seed: deterministic pseudo-random input, no wall clock
    for (int i = 0; i < 4000; ++i) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        const double u = static_cast<double>(s >> 40) / 1099511627776.0;  // [0, 1)
        a.push_back(static_cast<float>((u * 2.0 - 1.0) * 0.9));           // +-0.9: crosses the -3 dBFS ceiling
    }
    std::vector<float> b = a;
    const std::vector<float> in = a;
    op::limiter::Stats s1, s2;
    std::string err1, err2;
    const bool ok1 = op::limiter::apply(a, 2, 48000, -3.0, s1, err1);
    const bool ok2 = op::limiter::apply(b, 2, 48000, -3.0, s2, err2);
    CHECK(ok1 && ok2 && same_bytes(a, b), "determinism.output_bit_identical");
    CHECK(s1.frames == s2.frames && s1.sample_rate == s2.sample_rate &&
              s1.look_ahead_samples == s2.look_ahead_samples && s1.engaged == s2.engaged &&
              s1.max_gain_reduction_db == s2.max_gain_reduction_db &&
              s1.first_reduction_frame == s2.first_reduction_frame,
          "determinism.stats_identical");

    // A third run on the same input reproduces the first result exactly (statelessness across calls).
    std::vector<float> c = in;
    op::limiter::Stats s3;
    std::string err3;
    const bool ok3 = op::limiter::apply(c, 2, 48000, -3.0, s3, err3);
    CHECK(ok3 && same_bytes(c, in) == false && same_bytes(c, a), "determinism.third_run_reproduces");
}

// Non-finite samples (NaN, +-inf, |x| > 1e30) are treated as 0.0f — both measured and applied.
static void test_non_finite_as_zero() {
    const float nan_v = std::nanf(""), inf_v = INFINITY, ninf_v = -INFINITY;
    std::vector<float> buf(1000, 0.0f);  // mono silence
    buf[500] = nan_v;
    buf[501] = inf_v;
    buf[502] = ninf_v;
    buf[503] = 1e31f;
    buf[504] = -1e31f;
    op::limiter::Stats st;
    std::string err;
    const bool ok = op::limiter::apply(buf, 1, 48000, -6.0, st, err);
    CHECK(ok && !st.engaged && st.first_reduction_frame == -1 && st.max_gain_reduction_db == 0.0,
          "nonfinite.idle_stats");

    bool all_zero = true;
    for (const float s : buf) {
        if (!(s == 0.0f)) all_zero = false;  // false for NaN and +-inf outputs too
    }
    CHECK(all_zero, "nonfinite.output_all_zero");

    // Stereo: a non-finite left sample must not register as a peak against the quiet right pad.
    std::vector<float> st2(static_cast<size_t>(2) * 1000u, 0.2f);
    st2[static_cast<size_t>(900) * 2u] = inf_v;
    op::limiter::Stats stats2;
    std::string err2;
    const bool ok2 = op::limiter::apply(st2, 2, 48000, -6.0, stats2, err2);
    bool sanitized_ok = ok2 && !stats2.engaged && st2[static_cast<size_t>(900) * 2u] == 0.0f;
    for (long long n = 0; n < 1000; ++n) {
        if (n == 900) continue;
        if (st2[static_cast<size_t>(n) * 2u] != 0.2f || st2[static_cast<size_t>(n) * 2u + 1u] != 0.2f) {
            sanitized_ok = false;
        }
    }
    CHECK(sanitized_ok, "nonfinite.stereo_counts_as_zero");
}

int main() {
    test_validation_messages();
    test_zero_frames_idle_stats();
    test_look_ahead_samples();
    test_below_ceiling_bit_identical();
    test_impulse_in_silence();
    test_stereo_link();
    test_sine_settles_at_target_gain();
    test_attack_vs_release();
    test_determinism();
    test_non_finite_as_zero();
    std::printf("op_limiter_tests: %d failure(s)\n", g_failures);
    return g_failures;
}
