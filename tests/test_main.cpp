// Unit tests for the deterministic core. Boring assert-based runner — no framework deps.
// Exit code = number of failures. Run: ./build/op_tests
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "engine/dsp.hpp"
#include "engine/model.hpp"
#include "engine/render.hpp"
#include "engine/rng.hpp"
#include "engine/wav.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

static void write_text(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    f << text;
}

static void test_pcm16() {
    using op::wav::float_to_pcm16;
    CHECK(float_to_pcm16(0.0f) == 0, "pcm16.zero");
    CHECK(float_to_pcm16(-1.0f) == -32768, "pcm16.floor");
    CHECK(float_to_pcm16(1.0f) == 32767 && float_to_pcm16(5.0f) == 32767, "pcm16.saturation");
    CHECK(float_to_pcm16(std::nanf("")) == 0, "pcm16.nan_maps_zero");
    // half-to-even: scaled exactly .5 cases
    CHECK(float_to_pcm16(0.5f / 32768.0f) == 0, "pcm16.half_even_down_to_0");   // 0.5 → 0 (even)
    CHECK(float_to_pcm16(1.5f / 32768.0f) == 2, "pcm16.half_even_up_to_2");     // 1.5 → 2 (even)
    CHECK(float_to_pcm16(2.5f / 32768.0f) == 2, "pcm16.half_even_down_to_2");   // 2.5 → 2 (even)
    CHECK(float_to_pcm16(0.75f / 32768.0f) == 1 && float_to_pcm16(0.9f / 32768.0f) == 1, "pcm16.basic_rounding");
}

static void test_rng() {
    op::rng::Pcg32 a(42), b(42), c(43);
    bool same = true, differ = false;
    for (int i = 0; i < 10000; ++i) {
        const uint32_t va = a.next(), vb = b.next(), vc = c.next();
        if (va != vb) same = false;
        if (va != vc) differ = true;
    }
    CHECK(same, "rng.same_seed_identical_sequence");
    CHECK(differ, "rng.different_seeds_differ");
    op::rng::Pcg32 r(7);
    bool in_range = true;
    for (int i = 0; i < 1000; ++i) { const float f = r.next_float01(); if (!(f >= 0.0f && f < 1.0f)) in_range = false; }
    CHECK(in_range, "rng.float01_range");
}

static void test_dsp() {
    using namespace op::dsp;
    CHECK(std::fabs(db_to_lin(0.0f) - 1.0f) < 1e-6f, "db_to_lin.zero_db_is_unity");
    CHECK(std::fabs(db_to_lin(-6.0206f) - 0.5f) < 1e-4f, "db_to_lin.minus6db_is_half");
    float l = 0, r = 0;
    equal_power_pan(-1.0f, l, r);
    CHECK(std::fabs(r) < 1e-6f && std::fabs(l - 1.0f) < 1e-6f, "pan.full_left_silences_right");
    equal_power_pan(1.0f, l, r);
    CHECK(std::fabs(l) < 1e-6f && std::fabs(r - 1.0f) < 1e-6f, "pan.full_right_silences_left");
    equal_power_pan(0.0f, l, r);
    CHECK(std::fabs(l - r) < 1e-5f, "pan.center_equalish");
}

static void test_model() {
    const std::string good = R"({
      "format_version": 0, "bpm": 90.0, "sample_rate": 48000, "seed": 7,
      "length_bars": 1.5, "beats_per_bar": 4,
      "tracks": [{"name":"T","gain_db":-3.0,"pan":0.25,"voice":{"kind":"sine","freq_hz":220.0}}],
      "patterns": [{"track":0,"notes":[{"t":0.0,"dur":0.5},{"t":1.0,"dur":0.5}]}]
    })";
    write_text("/tmp/op_test_good.opj", good);
    op::Project p;
    std::string err;
    CHECK(op::load_project("/tmp/op_test_good.opj", p, err), "model.parse_valid");
    CHECK(p.bpm == 90.0 && p.seed == 7 && p.tracks.size() == 1 && p.patterns.size() == 1 &&
              p.patterns[0].notes.size() == 2,
          "model.fields_correct");

    write_text("/tmp/op_test_badver.opj", R"({"format_version": 9})");
    CHECK(!op::load_project("/tmp/op_test_badver.opj", p, err), "model.rejects_unknown_format_version");

    write_text("/tmp/op_test_oob.opj", R"({
      "format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[{"track":5,"notes":[]}]})");
    CHECK(!op::load_project("/tmp/op_test_oob.opj", p, err), "model.rejects_pattern_track_out_of_range");

    write_text("/tmp/op_test_voice.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"koto"}}]})");
    CHECK(!op::load_project("/tmp/op_test_voice.opj", p, err), "model.rejects_unknown_voice_kind");

    // arrangement (WP-0101): valid parse, defaults, validation rejections
    write_text("/tmp/op_test_arr.opj", R"({
        "format_version":0,"bpm":120,"tracks":[{"voice":{"kind":"metronome"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}],
        "arrangement":[{"pattern":0,"start_bar":1.5,"repeats":3},{"pattern":0}]})");
    op::Project pa;
    CHECK(op::load_project("/tmp/op_test_arr.opj", pa, err), "model.parse_arrangement");
    CHECK(pa.arrangement.size() == 2 && pa.arrangement[0].start_bar == 1.5 &&
          pa.arrangement[0].repeats == 3 && pa.arrangement[1].start_bar == 0.0 &&
          pa.arrangement[1].repeats == 1, "model.arrangement_defaults");
    write_text("/tmp/op_test_arr_oob.opj", R"({
        "format_version":0,"tracks":[{"voice":{"kind":"metronome"}}],
        "patterns":[{"track":0,"notes":[]}],"arrangement":[{"pattern":1}]})");
    CHECK(!op::load_project("/tmp/op_test_arr_oob.opj", pa, err), "model.rejects_clip_pattern_out_of_range");
    write_text("/tmp/op_test_arr_rep.opj", R"({
        "format_version":0,"tracks":[{"voice":{"kind":"metronome"}}],
        "patterns":[{"track":0,"notes":[]}],"arrangement":[{"pattern":0,"repeats":0}]})");
    CHECK(!op::load_project("/tmp/op_test_arr_rep.opj", pa, err), "model.rejects_zero_repeats");
    // regression: load_project must fully reset `out` (dirty reuse must not accumulate patterns)
    op::Project pd;
    CHECK(op::load_project("/tmp/op_test_arr.opj", pd, err) && pd.patterns.size() == 1, "model.dirty_pre");
    CHECK(op::load_project("/tmp/op_test_arr.opj", pd, err) && pd.patterns.size() == 1 &&
          pd.arrangement.size() == 2, "model.load_resets_out_object");

    // save/load round-trip
    op::Project q;
    q.bpm = 123.5; q.length_bars = 3.0; q.seed = 99;
    op::Track t; t.name = "RT"; t.voice.kind = op::VoiceKind::Sine; t.voice.freq_hz = 330.0f;
    q.tracks.push_back(t);
    op::Pattern pat; pat.track = 0; pat.notes.push_back({0.0, 0.5});
    q.patterns.push_back(pat);
    CHECK(op::save_project("/tmp/op_test_rt.opj", q, err), "model.save_ok");
    op::Project back;
    CHECK(op::load_project("/tmp/op_test_rt.opj", back, err) && back.bpm == 123.5 && back.seed == 99 &&
              back.tracks[0].voice.freq_hz == 330.0f,
          "model.roundtrip_stable");
}

static void test_wav_roundtrip() {
    op::wav::StereoBuffer buf;
    buf.sample_rate = 48000;
    const float vals[] = {0.0f, -1.0f, 0.5f, -0.25f, 0.999f, -0.999f};
    for (float v : vals) buf.interleaved.push_back(v), buf.interleaved.push_back(-v);
    std::string err;
    CHECK(op::wav::write_pcm16_stereo("/tmp/op_test_rt.wav", buf, err), "wav.write_ok");

    // header is canonical 44 bytes little-endian PCM16 stereo
    std::ifstream f("/tmp/op_test_rt.wav", std::ios::binary);
    unsigned char h[44];
    f.read(reinterpret_cast<char*>(h), 44);
    CHECK(std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVE", 4) == 0, "wav.header_riff_wave");
    CHECK(h[20] == 1 && h[22] == 2 && h[34] == 16, "wav.header_pcm16_stereo");
    CHECK((h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24)) == 48000, "wav.header_rate_48k");

    auto rr = op::wav::read_wav("/tmp/op_test_rt.wav");
    bool close = rr.ok && rr.samples.size() == sizeof(vals) / sizeof(float) * 2;
    for (size_t i = 0; close && i < rr.samples.size(); ++i) {
        const float expected = (i % 2 == 0) ? vals[i / 2] : -vals[i / 2];
        if (std::fabs(rr.samples[i] - expected) > 1.5f / 32768.0f) close = false;
    }
    CHECK(close, "wav.readback_within_1_5_lsb");
}

static void test_render() {
    op::Project p;
    p.bpm = 120.0; p.length_bars = 4.0; p.beats_per_bar = 4; p.sample_rate = 48000;
    CHECK(op::total_samples_for(p) == 384000, "render.total_samples_8s_at_120bpm");

    op::Track metro; metro.name = "M"; metro.gain_db = 0.0f; metro.voice.kind = op::VoiceKind::Metronome;
    metro.voice.accent_every = 4;
    p.tracks.push_back(metro);
    op::Pattern pat; pat.track = 0;
    for (int b = 0; b < 16; ++b) pat.notes.push_back({static_cast<double>(b), 0.25});
    p.patterns.push_back(pat);

    op::wav::StereoBuffer a, b;
    std::string err;
    CHECK(op::render_project(p, "", a, err), "render.succeeds");
    CHECK(static_cast<long long>(a.frames()) == 384000, "render.length_matches_total_samples");
    CHECK(op::render_project(p, "", b, err), "render.rerun_succeeds");
    const bool bitexact = a.interleaved.size() == b.interleaved.size() &&
                          std::memcmp(a.interleaved.data(), b.interleaved.data(),
                                      a.interleaved.size() * sizeof(float)) == 0;
    CHECK(bitexact, "render.determinism_bit_identical_rerun");

    // metronome energy: click window loud, gap quiet
    auto rms = [&](long long from, long long to) {
        double s = 0;
        for (long long i = from; i < to; ++i) s += a.interleaved[i * 2] * a.interleaved[i * 2];
        return std::sqrt(s / static_cast<double>(to - from));
    };
    const long long click_len = static_cast<long long>(0.030 * 48000);
    CHECK(rms(0, click_len) > 0.1, "render.click_window_has_energy");
    CHECK(rms(click_len + 2400, click_len + 7200) < 0.001, "render.gap_window_silent");

    // accent vs normal: zero-crossing rate ratio ≈ 1000/800
    auto zc = [&](long long from, long long to) {
        int n = 0;
        for (long long i = from + 1; i < to; ++i)
            if ((a.interleaved[i * 2] >= 0) != (a.interleaved[(i - 1) * 2] >= 0)) ++n;
        return n;
    };
    const long long beat1 = static_cast<long long>(1.0 * 48000 * 0.5);  // 1 beat @120bpm = 0.5s
    const double ratio = static_cast<double>(zc(0, click_len)) / static_cast<double>(zc(beat1, beat1 + click_len));
    CHECK(std::fabs(ratio - 1000.0 / 800.0) < (1000.0 / 800.0) * 0.15, "render.accent_freq_ratio_1000_over_800");

    // pan hard-right: left silent
    op::Project pr;
    pr.bpm = 120; pr.length_bars = 1; pr.sample_rate = 48000;
    op::Track tr; tr.name = "R"; tr.pan = 1.0f; tr.voice.kind = op::VoiceKind::Sine; tr.voice.freq_hz = 440;
    pr.tracks.push_back(tr);
    op::Pattern pnr; pnr.track = 0; pnr.notes.push_back({0.0, 3.5});
    pr.patterns.push_back(pnr);
    op::wav::StereoBuffer rb;
    CHECK(op::render_project(pr, "", rb, err), "render.panned_succeeds");
    bool left_silent = true;
    for (size_t i = 0; i < rb.frames(); ++i) if (rb.interleaved[i * 2] != 0.0f) left_silent = false;
    CHECK(left_silent, "render.pan_right_left_channel_exactly_zero");

    // --- stems export: sum of solo stems must be BIT-IDENTICAL to full mix (same addition order)
    {
        op::Project ps;
        ps.bpm = 100; ps.length_bars = 1.5; ps.sample_rate = 48000;
        op::Track t1; t1.name = "Sine"; t1.pan = -0.6f; t1.gain_db = -3.0f;
        t1.voice.kind = op::VoiceKind::Sine; t1.voice.freq_hz = 220.0f;
        op::Track t2; t2.name = "Metro"; t2.gain_db = -10.0f;
        t2.voice.kind = op::VoiceKind::Metronome; t2.voice.accent_every = 3;
        op::Track t3; t3.name = "Sub"; t3.pan = 0.8f;
        t3.voice.kind = op::VoiceKind::Sine; t3.voice.freq_hz = 55.0f;
        ps.tracks = {t1, t2, t3};
        op::Pattern a; a.track = 0; a.notes.push_back({0.0, 4.0});
        op::Pattern b; b.track = 1; for (int i = 0; i < 6; ++i) b.notes.push_back({static_cast<double>(i), 0.25});
        op::Pattern c; c.track = 2; c.notes.push_back({1.0, 3.0});
        ps.patterns = {a, b, c};

        std::vector<op::wav::StereoBuffer> stems;
        op::wav::StereoBuffer mix_buf;
        CHECK(op::render_stems(ps, "", stems, mix_buf, err), "stems.render_ok");
        CHECK(stems.size() == 3, "stems.one_per_track");
        bool sizes_ok = true;
        for (const auto& s : stems) if (s.frames() != mix_buf.frames()) sizes_ok = false;
        CHECK(sizes_ok, "stems.frame_lengths_match");
        std::vector<float> summed(mix_buf.interleaved.size(), 0.0f);
        for (const auto& s : stems)
            for (size_t i = 0; i < summed.size(); ++i) summed[i] += s.interleaved[i];
        CHECK(std::memcmp(summed.data(), mix_buf.interleaved.data(), summed.size() * sizeof(float)) == 0,
              "stems.sum_equals_mix_bit_exact");
    }
}

// --- arrangement clips (WP-0101): legacy equivalence + placement semantics.
// bpm=60, sr=48000 → exactly 48000 samples/beat, 192000/bar; note dur 0.5 beat = 24000 samples.
static void test_arrangement() {
    op::Project pr;
    pr.bpm = 60.0; pr.length_bars = 4.0; pr.sample_rate = 48000; pr.seed = 7;
    op::Track t1; t1.name = "S";
    t1.voice.kind = op::VoiceKind::Sine; t1.voice.freq_hz = 440.0f;
    pr.tracks = {t1};
    op::Pattern pat; pat.track = 0; pat.notes.push_back({0.0, 0.5});  // extent 0.5 beat
    pr.patterns = {pat};

    std::string err;
    op::wav::StereoBuffer legacy, explicit_default;
    CHECK(op::render_project(pr, "", legacy, err), "arr.legacy_render_ok");
    pr.arrangement = {{0, 0.0, 1}};
    CHECK(op::render_project(pr, "", explicit_default, err), "arr.default_clip_render_ok");
    CHECK(std::memcmp(legacy.interleaved.data(), explicit_default.interleaved.data(),
                      legacy.interleaved.size() * sizeof(float)) == 0,
          "arr.clip_0_at_bar0_equals_legacy_bit_exact");

    auto mean_sq = [](const op::wav::StereoBuffer& b, long long a, long long z) {
        double s = 0.0;
        for (long long i = a * 2; i < z * 2 && i + 1 < static_cast<long long>(b.interleaved.size()); i += 2)
            s += double(b.interleaved[i]) * b.interleaved[i];
        return s / double(z - a);
    };

    // offset: one clip at bar 1 → nothing before 192000, tone inside [192000, 216000)
    op::Project po = pr;
    po.arrangement = {{0, 1.0, 1}};
    op::wav::StereoBuffer ob;
    CHECK(op::render_project(po, "", ob, err), "arr.offset_render_ok");
    CHECK(mean_sq(ob, 0, 190000) == 0.0, "arr.silent_before_clip");
    CHECK(mean_sq(ob, 192300, 214000) > 0.01, "arr.sound_at_clip_start_bar1");

    // repeats: three statements spaced by extent (24000 samples): [0..72000), then silence
    op::Project rp = pr;
    rp.arrangement = {{0, 0.0, 3}};
    op::wav::StereoBuffer rb;
    CHECK(op::render_project(rp, "", rb, err), "arr.repeats_render_ok");
    CHECK(mean_sq(rb, 300, 23000) > 0.01, "arr.repeat_1_present");
    CHECK(mean_sq(rb, 48300, 70000) > 0.01, "arr.repeat_3_present");
    CHECK(mean_sq(rb, 72100, 96000) == 0.0, "arr.silence_after_repeats");

    // unreferenced pattern is silent when arrangement is present (documented semantics)
    op::Project pu = pr;
    op::Pattern ghost; ghost.track = 0; ghost.notes.push_back({20.0, 0.5});  // would land past window anyway
    pu.patterns.push_back(ghost);
    pu.arrangement = {{0, 0.0, 1}};  // references pattern 0 only
    op::wav::StereoBuffer ub;
    CHECK(op::render_project(pu, "", ub, err), "arr.unreferenced_render_ok");
    CHECK(std::memcmp(ub.interleaved.data(), legacy.interleaved.data(),
                      legacy.interleaved.size() * sizeof(float)) == 0,
          "arr.unreferenced_pattern_silent_bit_exact");
}

int main() {
    test_pcm16();
    test_rng();
    test_dsp();
    test_model();
    test_wav_roundtrip();
    test_render();
    test_arrangement();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
