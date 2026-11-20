// Unit tests for op::peaks (WP-0171, docs/spec/peaks.md). Boring assert-based runner — no framework deps.
// Exit code = number of failures. Run: ./build/op_peaks_tests
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/peaks.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

// Hand-computed mono case: 8 frames into 4 bins -> each bin covers exactly 2 frames.
static void test_mono_8_frames_4_bins() {
    const std::vector<float> x = {0.5f, -0.25f, 0.75f, 0.1f, -0.9f, 0.3f, 0.6f, -0.6f};
    const op::peaks::Peaks p = op::peaks::compute_peaks(x, 1, 48000, 4);
    CHECK(p.frames == 8 && p.bins == 4 && p.min.size() == 1 && p.min[0].size() == 4 && p.max.size() == 1,
          "mono8x4.shape");
    CHECK(p.min[0][0] == -0.25f && p.max[0][0] == 0.5f, "mono8x4.bin0");   // frames 0..1: 0.5, -0.25
    CHECK(p.min[0][1] == 0.1f && p.max[0][1] == 0.75f, "mono8x4.bin1");    // frames 2..3: 0.75, 0.1
    CHECK(p.min[0][2] == -0.9f && p.max[0][2] == 0.3f, "mono8x4.bin2");    // frames 4..5: -0.9, 0.3
    CHECK(p.min[0][3] == -0.6f && p.max[0][3] == 0.6f, "mono8x4.bin3");    // frames 6..7: 0.6, -0.6
}

// Stereo case: 3 frames (6 interleaved samples) into 1 bin -> per-channel min/max over all frames.
static void test_stereo_3_frames_1_bin() {
    const std::vector<float> x = {0.2f, -0.4f, 0.9f, 0.1f, -0.7f, -0.8f};  // L,R,L,R,L,R
    const op::peaks::Peaks p = op::peaks::compute_peaks(x, 2, 44100, 1);
    CHECK(p.frames == 3 && p.bins == 1 && p.min.size() == 2 && p.min[0].size() == 1 && p.max.size() == 2,
          "stereo3x1.shape");
    CHECK(p.min[0][0] == -0.7f && p.max[0][0] == 0.9f, "stereo3x1.left");   // L: 0.2, 0.9, -0.7
    CHECK(p.min[1][0] == -0.8f && p.max[1][0] == 0.1f, "stereo3x1.right");  // R: -0.4, 0.1, -0.8
}

// Fewer frames than bins: 2 mono frames into 5 bins -> frame layout 0,0,0,1,1 (spec §Definitions).
static void test_fewer_frames_than_bins() {
    const std::vector<float> x = {0.3f, -0.5f};
    const op::peaks::Peaks p = op::peaks::compute_peaks(x, 1, 8000, 5);
    CHECK(p.frames == 2 && p.bins == 5 && p.min[0].size() == 5, "few_frames.shape");
    const float want[5] = {0.3f, 0.3f, 0.3f, -0.5f, -0.5f};
    bool min_ok = true, max_ok = true;
    for (int b = 0; b < 5; ++b) {
        if (p.min[0][b] != want[b]) min_ok = false;
        if (p.max[0][b] != want[b]) max_ok = false;
    }
    CHECK(min_ok, "few_frames.min_layout_00011");
    CHECK(max_ok, "few_frames.max_layout_00011");
}

// Empty input: frames == 0 -> every value is 0.0f with the requested bin count (spec §Definitions).
static void test_empty_input() {
    const std::vector<float> x;
    const op::peaks::Peaks p = op::peaks::compute_peaks(x, 2, 48000, 4);
    bool all_zero = p.frames == 0 && p.bins == 4 && p.min.size() == 2 && p.max.size() == 2;
    for (size_t c = 0; c < p.min.size() && all_zero; ++c)
        for (int b = 0; b < p.bins; ++b)
            if (p.min[c][b] != 0.0f || p.max[c][b] != 0.0f) all_zero = false;
    CHECK(all_zero, "empty_input.zeros");
}

// Invalid arguments: bins == 0, frames == 0, empty min/max; sample_rate/channels still set (spec).
static void test_invalid_arguments() {
    const std::vector<float> x = {0.1f, 0.2f};
    const op::peaks::Peaks c3 = op::peaks::compute_peaks(x, 3, 48000, 8);
    CHECK(c3.bins == 0 && c3.frames == 0 && c3.min.empty() && c3.max.empty() &&
              c3.sample_rate == 48000 && c3.channels == 3,
          "invalid.channels_3");
    const op::peaks::Peaks c0 = op::peaks::compute_peaks(x, 0, 48000, 8);
    CHECK(c0.bins == 0 && c0.frames == 0 && c0.min.empty() && c0.max.empty(), "invalid.channels_0");
    const op::peaks::Peaks b0 = op::peaks::compute_peaks(x, 1, 48000, 0);
    CHECK(b0.bins == 0 && b0.frames == 0 && b0.min.empty() && b0.max.empty(), "invalid.bins_0");
    const op::peaks::Peaks bo = op::peaks::compute_peaks(x, 1, 48000, op::peaks::kMaxBins + 1);
    CHECK(bo.bins == 0 && bo.frames == 0 && bo.min.empty() && bo.max.empty(), "invalid.bins_over_max");
    const op::peaks::Peaks bm = op::peaks::compute_peaks(x, 1, 48000, op::peaks::kMaxBins);
    CHECK(bm.bins == op::peaks::kMaxBins && bm.min.size() == 1 &&
              static_cast<int>(bm.min[0].size()) == op::peaks::kMaxBins,
          "valid.bins_at_max");
}

// Non-finite samples (NaN, +-inf) and magnitudes beyond 1e30 count as 0.0f (spec §Definitions).
static void test_non_finite_as_zero() {
    const float nan_v = std::nanf(""), inf_v = INFINITY, ninf_v = -INFINITY;
    const std::vector<float> all_bad = {nan_v, inf_v, ninf_v};
    const op::peaks::Peaks p1 = op::peaks::compute_peaks(all_bad, 1, 48000, 1);
    CHECK(p1.min[0][0] == 0.0f && p1.max[0][0] == 0.0f, "nonfinite.all_bad_zero");
    const std::vector<float> mixed = {nan_v, 0.5f, ninf_v, 0.25f};  // sanitized: 0, 0.5, 0, 0.25
    const op::peaks::Peaks p2 = op::peaks::compute_peaks(mixed, 1, 48000, 1);
    CHECK(p2.min[0][0] == 0.0f && p2.max[0][0] == 0.5f, "nonfinite.mixed");
    const std::vector<float> huge = {1e31f, -1e31f};  // magnitude beyond 1e30 -> 0
    const op::peaks::Peaks p3 = op::peaks::compute_peaks(huge, 1, 48000, 1);
    CHECK(p3.min[0][0] == 0.0f && p3.max[0][0] == 0.0f, "nonfinite.huge_zero");
}

// Exact JSON text for a tiny case (compact line, %.6f values, terminated by '\n').
static void test_json_exact_text() {
    const std::vector<float> x = {0.5f, -0.25f};
    const op::peaks::Peaks p = op::peaks::compute_peaks(x, 1, 48000, 2);
    const std::string want =
        "{\"version\":1,\"sample_rate\":48000,\"channels\":1,\"frames\":2,\"bins\":2,"
        "\"min\":[[0.500000,-0.250000]],\"max\":[[0.500000,-0.250000]]}\n";
    CHECK(op::peaks::to_json(p) == want, "json.tiny_exact");
}

// Invalid-input Peaks prints "bins":0,"min":[],"max":[] (spec §Definitions).
static void test_json_invalid() {
    const std::vector<float> x = {0.1f};
    const op::peaks::Peaks p = op::peaks::compute_peaks(x, 3, 44100, 8);
    const std::string want =
        "{\"version\":1,\"sample_rate\":44100,\"channels\":3,\"frames\":0,\"bins\":0,\"min\":[],\"max\":[]}\n";
    CHECK(op::peaks::to_json(p) == want, "json.invalid_exact");
}

// The text "-0.000000" is never printed (spec §Definitions): -0.0f and tiny negatives print 0.000000.
static void test_json_no_negative_zero() {
    const std::vector<float> x = {-0.0f, -1e-9f};  // min = -1e-9 -> "-0.000000", max = -0.0 -> same
    const op::peaks::Peaks p = op::peaks::compute_peaks(x, 1, 48000, 1);
    const std::string json = op::peaks::to_json(p);
    CHECK(json.find("-0.000000") == std::string::npos, "json.no_negative_zero_text");
    const std::string want =
        "{\"version\":1,\"sample_rate\":48000,\"channels\":1,\"frames\":2,\"bins\":1,"
        "\"min\":[[0.000000]],\"max\":[[0.000000]]}\n";
    CHECK(json == want, "json.no_negative_zero_exact");
}

int main() {
    test_mono_8_frames_4_bins();
    test_stereo_3_frames_1_bin();
    test_fewer_frames_than_bins();
    test_empty_input();
    test_invalid_arguments();
    test_non_finite_as_zero();
    test_json_exact_text();
    test_json_invalid();
    test_json_no_negative_zero();
    std::printf("op_peaks_tests: %d failure(s)\n", g_failures);
    return g_failures;
}
