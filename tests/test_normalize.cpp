// Unit tests for engine/normalize (docs/spec/normalize.md, WP-0169). Boring assert-based runner —
// no framework deps. Exit code = number of failures. Run: ./build/op_normalize_tests
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "engine/analyze.hpp"
#include "engine/normalize.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace {

constexpr int kFs = 48000;

// Stereo sine buffer (interleaved L,R,L,R...), `frames` frames per channel. Deterministic closed form.
std::vector<float> stereo_sine(double amp, double freq_hz, long long frames) {
    std::vector<float> v(static_cast<size_t>(frames) * 2u, 0.0f);
    const double w = 2.0 * 3.14159265358979323846 * freq_hz / static_cast<double>(kFs);
    for (long long i = 0; i < frames; ++i) {
        const float s = static_cast<float>(amp * std::sin(w * static_cast<double>(i)));
        v[static_cast<size_t>(i) * 2u + 0u] = s;
        v[static_cast<size_t>(i) * 2u + 1u] = s;
    }
    return v;
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

double lufs_of(const std::vector<float>& v) {
    const op::analyze::Features f = op::analyze::analyze_samples(v, 2, kFs);
    return f.integrated_lufs ? *f.integrated_lufs : -1e9;
}

double peak_dbfs_of(const std::vector<float>& v) {
    const op::analyze::Features f = op::analyze::analyze_samples(v, 2, kFs);
    return f.peak_dbfs ? *f.peak_dbfs : -1e9;
}

}  // namespace

// An unlimited plan (loudness below target, peak well under the ceiling) reports limited == false and
// reaches the target after apply + re-analysis (within 0.01 LU).
static void test_plan_unlimited() {
    std::vector<float> v = stereo_sine(0.1, 1000.0, 3LL * kFs);
    op::normalize::GainPlan plan;
    std::string err;
    const bool ok = op::normalize::plan_gain(v, 2, kFs, /*target_lufs=*/-16.0, /*ceiling_dbfs=*/-1.0, plan, err);
    CHECK(ok && err.empty() && !plan.limited && near(plan.gain_db, plan.requested_gain_db, 1e-9),
          "plan.unlimited_fields");
    op::normalize::apply_gain_db(v, plan.gain_db);
    CHECK(near(lufs_of(v), -16.0, 0.01), "plan.unlimited_reaches_target");
}

// Loud input (peak ~-0.45 dBFS) toward 0 LUFS with ceiling -1: the ceiling must cap the gain and the
// re-analyzed sample peak must land on -1.0 +/- 0.01 dBFS.
static void test_plan_limited() {
    std::vector<float> v = stereo_sine(0.95, 1000.0, 3LL * kFs);
    op::normalize::GainPlan plan;
    std::string err;
    const bool ok = op::normalize::plan_gain(v, 2, kFs, /*target_lufs=*/0.0, /*ceiling_dbfs=*/-1.0, plan, err);
    CHECK(ok && err.empty() && plan.limited && plan.gain_db < plan.requested_gain_db,
          "plan.limited_fields");
    op::normalize::apply_gain_db(v, plan.gain_db);
    CHECK(near(peak_dbfs_of(v), -1.0, 0.01), "plan.limited_peak_at_ceiling");
}

// A peak above the ceiling is attenuated (gain_db < 0) even when loudness is already below target.
static void test_plan_negative_gain() {
    std::vector<float> v = stereo_sine(0.001, 1000.0, 3LL * kFs);  // quiet: ~-63 LUFS, far below 0
    v[48000] = 0.9f;                                               // one loud sample: peak ~-0.9 dBFS
    op::normalize::GainPlan plan;
    std::string err;
    const bool ok = op::normalize::plan_gain(v, 2, kFs, /*target_lufs=*/0.0, /*ceiling_dbfs=*/-1.0, plan, err);
    CHECK(ok && err.empty() && plan.limited && plan.gain_db < 0.0, "plan.negative_gain_attenuates");
}

// Each failure returns false with exactly its spec error string (checked in spec order).
static void test_plan_errors() {
    const std::vector<float> v = stereo_sine(0.1, 1000.0, 2LL * kFs);
    op::normalize::GainPlan plan;
    std::string err;

    bool ok = op::normalize::plan_gain(v, 2, kFs, 10.0, -1.0, plan, err);
    CHECK(!ok && err == "target LUFS out of range [-70, 0]", "err.target_above_range");
    err.clear();
    ok = op::normalize::plan_gain(v, 2, kFs, -71.0, -1.0, plan, err);
    CHECK(!ok && err == "target LUFS out of range [-70, 0]", "err.target_below_range");
    err.clear();
    ok = op::normalize::plan_gain(v, 2, kFs, std::nan(""), -1.0, plan, err);
    CHECK(!ok && err == "target LUFS out of range [-70, 0]", "err.target_nan");
    err.clear();
    ok = op::normalize::plan_gain(v, 2, kFs, -16.0, -35.0, plan, err);
    CHECK(!ok && err == "ceiling out of range [-30, 0]", "err.ceiling_below_range");
    err.clear();
    ok = op::normalize::plan_gain(v, 2, kFs, -16.0, std::nan(""), plan, err);
    CHECK(!ok && err == "ceiling out of range [-30, 0]", "err.ceiling_nan");
    err.clear();

    const std::vector<float> silence(static_cast<size_t>(kFs) * 2u * 2u, 0.0f);  // 2 s of zeros
    ok = op::normalize::plan_gain(silence, 2, kFs, -16.0, -1.0, plan, err);
    CHECK(!ok && err == "no measurable loudness (silent or too short)", "err.silent");
    err.clear();

    const std::vector<float> short_buf = stereo_sine(0.5, 1000.0, kFs / 100);  // 10 ms: no full block
    ok = op::normalize::plan_gain(short_buf, 2, kFs, -16.0, -1.0, plan, err);
    CHECK(!ok && err == "no measurable loudness (silent or too short)", "err.too_short");
}

// 0 dB multiplies by exactly 1.0f: every sample stays bit-identical.
static void test_apply_zero_identity() {
    std::vector<float> v = {0.0f, -0.0f, 1.0f, -1.0f, 0.125f, -1e20f, 3.4028234663852886e38f,
                            std::numeric_limits<float>::denorm_min(), 0.5f, -0.75f};
    const std::vector<float> copy = v;
    op::normalize::apply_gain_db(v, 0.0);
    CHECK(std::memcmp(v.data(), copy.data(), v.size() * sizeof(float)) == 0, "apply.zero_bit_identical");
}

// +20 dB multiplies by ~10 (the factor is exactly 10.0f: pow(10, 1) is exact).
static void test_apply_twenty_db() {
    std::vector<float> v = {0.0f, 0.5f, -0.25f, 0.1f, -3.0f};
    const std::vector<float> copy = v;
    op::normalize::apply_gain_db(v, 20.0);
    bool times_ten = true;
    for (size_t i = 0; i < v.size(); ++i) {
        const double want = static_cast<double>(copy[i]) * 10.0;
        if (!(std::fabs(static_cast<double>(v[i]) - want) <= std::fabs(want) * 1e-6 + 1e-30)) times_ten = false;
    }
    CHECK(times_ten, "apply.twenty_db_times_ten");
}

int main() {
    test_plan_unlimited();
    test_plan_limited();
    test_plan_negative_gain();
    test_plan_errors();
    test_apply_zero_identity();
    test_apply_twenty_db();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
