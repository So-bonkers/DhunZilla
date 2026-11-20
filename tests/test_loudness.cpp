// Unit tests for op::loudness (docs/spec/loudness.md, WP-0168): true peak (dBTP), momentary /
// short-term maximum LUFS and EBU Tech 3342 loudness range. Boring assert-based runner — no framework
// deps, own main. Exit code = number of failures. Run: ./build/op_loudness_tests
#include <cstdio>
#include <string>
#include <vector>

#include "engine/analyze.hpp"
#include "engine/dsp.hpp"
#include "engine/loudness.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

static bool near_(double got, double want, double tol) { return got >= want - tol && got <= want + tol; }

// Stereo sine, both channels identical: x[i] = amp·sin(2π·f·i/fs + phase).
static std::vector<float> make_sine_stereo(double amp, double freq_hz, long long frames, double fs,
                                           double phase = 0.0) {
    std::vector<float> x(static_cast<size_t>(frames) * 2u);
    for (long long i = 0; i < frames; ++i) {
        const double s = amp * op::dsp::sind_(2.0 * op::dsp::kPi_d() * freq_hz *
                                                  static_cast<double>(i) / fs + phase);
        x[static_cast<size_t>(i) * 2u] = static_cast<float>(s);
        x[static_cast<size_t>(i) * 2u + 1u] = static_cast<float>(s);
    }
    return x;
}

static std::vector<float> make_sine_mono(double amp, double freq_hz, long long frames, double fs,
                                         double phase = 0.0) {
    std::vector<float> x(static_cast<size_t>(frames));
    for (long long i = 0; i < frames; ++i)
        x[static_cast<size_t>(i)] = static_cast<float>(
            amp * op::dsp::sind_(2.0 * op::dsp::kPi_d() * freq_hz * static_cast<double>(i) / fs + phase));
    return x;
}

// Largest |sample| over every sample of every channel (sample peak, not true peak), in dBFS.
static double sample_peak_db(const std::vector<float>& x) {
    double mx = 0.0;
    for (float v : x) {
        const double a = v < 0.0f ? -static_cast<double>(v) : static_cast<double>(v);
        if (a > mx) mx = a;
    }
    return mx == 0.0 ? -1000.0 : 20.0 * op::dsp::log10d_(mx);
}

int main() {
    using op::loudness::measure;
    using op::loudness::to_json;
    const int fs = 48000;

    // --- inter-sample peak: full-scale sine at fs/4, phase π/4 → every sample sits at ±1/√2, i.e. the
    // sample peak is −3.01 dBFS while the true peak is 0 dBTP (the classic "no headroom left" case).
    const long long n_is = 48000;  // 1 s is plenty: many periods of fs/4
    auto is4 = make_sine_stereo(1.0, static_cast<double>(fs) / 4.0, n_is, static_cast<double>(fs),
                                op::dsp::kPi_d() / 4.0);
    const double sp_is = sample_peak_db(is4);
    auto ris = measure(is4, 2, fs);
    CHECK(near_(sp_is, -3.0103, 0.02), "loudness.tp_fs4_sine_sample_peak_is_minus3_01");
    CHECK(ris.true_peak_dbtp && near_(*ris.true_peak_dbtp, 0.0, 0.3) &&
          *ris.true_peak_dbtp - sp_is > 2.5, "loudness.tp_fs4_sine_reports_0_dbtp_far_above_sample_peak");

    // --- low-frequency sine: no inter-sample gain, true peak ≈ sample peak ≈ −6.02 dBFS (amp 0.5).
    auto low = make_sine_stereo(0.5, 100.0, 48000, static_cast<double>(fs));
    auto rlow = measure(low, 2, fs);
    CHECK(rlow.true_peak_dbtp && near_(*rlow.true_peak_dbtp, -6.0206, 0.05) &&
          near_(*rlow.true_peak_dbtp, sample_peak_db(low), 0.01),
          "loudness.tp_low_freq_sine_matches_sample_peak_minus6_02");

    // --- digital silence: frames/duration are reported, every optional is empty.
    std::vector<float> zeros(static_cast<size_t>(fs) * 2u, 0.0f);
    auto rz = measure(zeros, 2, fs);
    CHECK(!rz.true_peak_dbtp && !rz.momentary_max_lufs && !rz.short_term_max_lufs && !rz.loudness_range_lu &&
          rz.frames == fs && near_(rz.duration_s, 1.0, 1e-12),
          "loudness.silence_all_optionals_empty");

    // --- empty input and degenerate arguments: no measurements, nothing crashes.
    std::vector<float> none;
    auto re = measure(none, 2, fs);
    CHECK(!re.true_peak_dbtp && !re.momentary_max_lufs && !re.short_term_max_lufs && !re.loudness_range_lu &&
          re.frames == 0 && near_(re.duration_s, 0.0, 1e-12), "loudness.empty_input_all_optionals_empty");
    auto rbad = measure(make_sine_stereo(0.5, 440.0, 48000, static_cast<double>(fs)), 3, fs);
    auto rsr0 = measure(make_sine_stereo(0.5, 440.0, 48000, static_cast<double>(fs)), 2, 0);
    CHECK(!rbad.true_peak_dbtp && !rbad.momentary_max_lufs && !rsr0.true_peak_dbtp &&
          !rsr0.momentary_max_lufs && rsr0.frames == fs && near_(rsr0.duration_s, 0.0, 1e-12),
          "loudness.bad_channels_or_rate_returns_optionals_empty");

    // --- stationary tone: momentary max and short-term max both sit on the integrated loudness that
    // `opd analyze` reports (±0.05 LU), and a steady signal has essentially no loudness range.
    const double amp20 = op::dsp::powd_(10.0, -20.0 / 20.0);
    auto tone = make_sine_stereo(amp20, 997.0, 8 * static_cast<long long>(fs), static_cast<double>(fs));
    const op::analyze::Features feats = op::analyze::analyze_samples(tone, 2, fs);
    auto rt = measure(tone, 2, fs);
    CHECK(feats.integrated_lufs && rt.momentary_max_lufs && rt.short_term_max_lufs &&
          near_(*rt.momentary_max_lufs, *feats.integrated_lufs, 0.05) &&
          near_(*rt.short_term_max_lufs, *feats.integrated_lufs, 0.05),
          "loudness.stationary_tone_momentary_and_short_term_match_analyze_lufs");
    CHECK(rt.loudness_range_lu && *rt.loudness_range_lu < 0.1, "loudness.stationary_tone_loudness_range_below_0_1lu");

    // --- too short for any window (< 400 ms): no momentary / short-term / LRA, but still a true peak.
    auto short_in = make_sine_stereo(0.9, 997.0, fs / 10, static_cast<double>(fs));  // 100 ms
    auto rs = measure(short_in, 2, fs);
    CHECK(!rs.momentary_max_lufs && !rs.short_term_max_lufs && !rs.loudness_range_lu &&
          rs.true_peak_dbtp && near_(*rs.true_peak_dbtp, sample_peak_db(short_in), 0.3) &&
          rs.frames == fs / 10, "loudness.input_shorter_than_400ms_only_true_peak");

    // --- loudness range: two 12 s halves 10 dB apart → LRA ≈ 10 LU (P95 in the loud half, P10 quiet).
    const long long half = 12 * static_cast<long long>(fs);
    std::vector<float> step = make_sine_stereo(amp20, 997.0, half, static_cast<double>(fs));
    const std::vector<float> quiet =
        make_sine_stereo(amp20 * op::dsp::powd_(10.0, -10.0 / 20.0), 997.0, half, static_cast<double>(fs));
    step.insert(step.end(), quiet.begin(), quiet.end());
    auto rl = measure(step, 2, fs);
    CHECK(rl.loudness_range_lu && near_(*rl.loudness_range_lu, 10.0, 0.5),
          "loudness.lra_two_halves_10db_apart_is_10lu");

    // --- a 400 ms window can never be quieter than the 3 s window covering it: momentary max ≥ short-term max.
    CHECK(rt.momentary_max_lufs && rt.short_term_max_lufs &&
          *rt.momentary_max_lufs >= *rt.short_term_max_lufs &&
          rl.momentary_max_lufs && rl.short_term_max_lufs &&
          *rl.momentary_max_lufs >= *rl.short_term_max_lufs,
          "loudness.momentary_max_ge_short_term_max");

    // --- mono works end to end (single channel weight).
    auto mono = make_sine_mono(amp20, 997.0, 4 * static_cast<long long>(fs), static_cast<double>(fs));
    auto rm = measure(mono, 1, fs);
    CHECK(rm.channels == 1 && rm.frames == 4 * static_cast<long long>(fs) && rm.true_peak_dbtp &&
          rm.momentary_max_lufs && rm.short_term_max_lufs && rm.loudness_range_lu,
          "loudness.mono_input_measured");

    // --- to_json: byte-exact canonical text (numbers %.4f, duration %.6f), nulls for empty optionals.
    op::loudness::Result f;
    f.sample_rate = 48000; f.channels = 2; f.frames = 480000; f.duration_s = 10.0;
    f.true_peak_dbtp = -1.2345; f.momentary_max_lufs = -14.0; f.short_term_max_lufs = -14.1;
    f.loudness_range_lu = 3.2;
    CHECK(to_json(f) == "{\n"
                        "  \"sample_rate\": 48000,\n"
                        "  \"channels\": 2,\n"
                        "  \"frames\": 480000,\n"
                        "  \"duration_s\": 10.000000,\n"
                        "  \"true_peak_dbtp\": -1.2345,\n"
                        "  \"momentary_max_lufs\": -14.0000,\n"
                        "  \"short_term_max_lufs\": -14.1000,\n"
                        "  \"loudness_range_lu\": 3.2000\n"
                        "}\n", "loudness.to_json_matches_spec_sample_byte_exact");
    CHECK(to_json(rz) == "{\n"
                         "  \"sample_rate\": 48000,\n"
                         "  \"channels\": 2,\n"
                         "  \"frames\": 48000,\n"
                         "  \"duration_s\": 1.000000,\n"
                         "  \"true_peak_dbtp\": null,\n"
                         "  \"momentary_max_lufs\": null,\n"
                         "  \"short_term_max_lufs\": null,\n"
                         "  \"loudness_range_lu\": null\n"
                         "}\n", "loudness.silence_json_prints_null");

    // --- determinism: measuring the same buffer twice yields byte-identical JSON.
    CHECK(to_json(measure(tone, 2, fs)) == to_json(rt), "loudness.repeat_measure_is_byte_identical");

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
