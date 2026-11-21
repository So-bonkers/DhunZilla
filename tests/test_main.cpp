// Unit tests for the deterministic core. Boring assert-based runner — no framework deps.
// Exit code = number of failures. Run: ./build/op_tests
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <unistd.h>

#include "doc/document.hpp"
#include "engine/analyze.hpp"
#include "engine/bundle.hpp"
#include "engine/dsp.hpp"
#include "engine/midi.hpp"
#include "engine/model.hpp"
#include "engine/model_json.hpp"
#include "engine/ops.hpp"
#include "engine/render.hpp"
#include "engine/resample.hpp"
#include "engine/rng.hpp"
#include "engine/sample_index.hpp"
#include "engine/wav.hpp"

#include "player.hpp"             // WP-0153: dz::Player (dzplayer, JUCE-free)
#include "render_playback.hpp"    // WP-0153: dz::format_position

#include "resample_reference.hpp"  // frozen pre-WP-0150 direct algorithm (bit-compare target)

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

// Per-process scratch directory: <temp_directory_path()>/op_tests_<pid>. Created (after removing any
// stale one with that name) on first call; the same string is returned on every later call.
static std::string tmp_dir() {
    static const std::string dir = [] {
        const std::filesystem::path p = std::filesystem::temp_directory_path() /
                                        ("op_tests_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;  // errors ignored: the tests below report what they cannot write
        std::filesystem::remove_all(p, ec);
        std::filesystem::create_directories(p, ec);
        return p.string();
    }();
    return dir;
}

// Path of `name` inside tmp_dir(): tmp_dir() + "/" + name. `name` may contain '/' (e.g. "op_bd/kits");
// the helper does NOT create sub-directories — tests keep creating their own, as they do today.
static std::string tmp_file(const std::string& name) { return tmp_dir() + "/" + name; }

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
    write_text(tmp_file("op_test_good.opj"), good);
    op::Project p;
    std::string err;
    CHECK(op::load_project(tmp_file("op_test_good.opj"), p, err), "model.parse_valid");
    CHECK(p.bpm == 90.0 && p.seed == 7 && p.tracks.size() == 1 && p.patterns.size() == 1 &&
              p.patterns[0].notes.size() == 2,
          "model.fields_correct");

    write_text(tmp_file("op_test_badver.opj"), R"({"format_version": 9})");
    CHECK(!op::load_project(tmp_file("op_test_badver.opj"), p, err), "model.rejects_unknown_format_version");

    write_text(tmp_file("op_test_oob.opj"), R"({
      "format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[{"track":5,"notes":[]}]})");
    CHECK(!op::load_project(tmp_file("op_test_oob.opj"), p, err), "model.rejects_pattern_track_out_of_range");

    write_text(tmp_file("op_test_voice.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"koto"}}]})");
    CHECK(!op::load_project(tmp_file("op_test_voice.opj"), p, err), "model.rejects_unknown_voice_kind");

    // arrangement (WP-0101): valid parse, defaults, validation rejections
    write_text(tmp_file("op_test_arr.opj"), R"({
        "format_version":0,"bpm":120,"tracks":[{"voice":{"kind":"metronome"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}],
        "arrangement":[{"pattern":0,"start_bar":1.5,"repeats":3},{"pattern":0}]})");
    op::Project pa;
    CHECK(op::load_project(tmp_file("op_test_arr.opj"), pa, err), "model.parse_arrangement");
    CHECK(pa.arrangement.size() == 2 && pa.arrangement[0].start_bar == 1.5 &&
          pa.arrangement[0].repeats == 3 && pa.arrangement[1].start_bar == 0.0 &&
          pa.arrangement[1].repeats == 1, "model.arrangement_defaults");
    write_text(tmp_file("op_test_arr_oob.opj"), R"({
        "format_version":0,"tracks":[{"voice":{"kind":"metronome"}}],
        "patterns":[{"track":0,"notes":[]}],"arrangement":[{"pattern":1}]})");
    CHECK(!op::load_project(tmp_file("op_test_arr_oob.opj"), pa, err), "model.rejects_clip_pattern_out_of_range");
    write_text(tmp_file("op_test_arr_rep.opj"), R"({
        "format_version":0,"tracks":[{"voice":{"kind":"metronome"}}],
        "patterns":[{"track":0,"notes":[]}],"arrangement":[{"pattern":0,"repeats":0}]})");
    CHECK(!op::load_project(tmp_file("op_test_arr_rep.opj"), pa, err), "model.rejects_zero_repeats");
    // regression: load_project must fully reset `out` (dirty reuse must not accumulate patterns)
    op::Project pd;
    CHECK(op::load_project(tmp_file("op_test_arr.opj"), pd, err) && pd.patterns.size() == 1, "model.dirty_pre");
    CHECK(op::load_project(tmp_file("op_test_arr.opj"), pd, err) && pd.patterns.size() == 1 &&
          pd.arrangement.size() == 2, "model.load_resets_out_object");

    // save/load round-trip
    op::Project q;
    q.bpm = 123.5; q.length_bars = 3.0; q.seed = 99;
    op::Track t; t.name = "RT"; t.voice.kind = op::VoiceKind::Sine; t.voice.freq_hz = 330.0f;
    q.tracks.push_back(t);
    op::Pattern pat; pat.track = 0; pat.notes.push_back({0.0, 0.5});
    q.patterns.push_back(pat);
    CHECK(op::save_project(tmp_file("op_test_rt.opj"), q, err), "model.save_ok");
    op::Project back;
    CHECK(op::load_project(tmp_file("op_test_rt.opj"), back, err) && back.bpm == 123.5 && back.seed == 99 &&
              back.tracks[0].voice.freq_hz == 330.0f,
          "model.roundtrip_stable");
}

static void test_wav_roundtrip() {
    op::wav::StereoBuffer buf;
    buf.sample_rate = 48000;
    const float vals[] = {0.0f, -1.0f, 0.5f, -0.25f, 0.999f, -0.999f};
    for (float v : vals) buf.interleaved.push_back(v), buf.interleaved.push_back(-v);
    std::string err;
    CHECK(op::wav::write_pcm16_stereo(tmp_file("op_test_rt.wav"), buf, err), "wav.write_ok");

    // header is canonical 44 bytes little-endian PCM16 stereo
    std::ifstream f(tmp_file("op_test_rt.wav"), std::ios::binary);
    unsigned char h[44];
    f.read(reinterpret_cast<char*>(h), 44);
    CHECK(std::memcmp(h, "RIFF", 4) == 0 && std::memcmp(h + 8, "WAVE", 4) == 0, "wav.header_riff_wave");
    CHECK(h[20] == 1 && h[22] == 2 && h[34] == 16, "wav.header_pcm16_stereo");
    CHECK((h[24] | (h[25] << 8) | (h[26] << 16) | (h[27] << 24)) == 48000, "wav.header_rate_48k");

    auto rr = op::wav::read_wav(tmp_file("op_test_rt.wav"));
    bool close = rr.ok && rr.samples.size() == sizeof(vals) / sizeof(float) * 2;
    for (size_t i = 0; close && i < rr.samples.size(); ++i) {
        const float expected = (i % 2 == 0) ? vals[i / 2] : -vals[i / 2];
        if (std::fabs(rr.samples[i] - expected) > 1.5f / 32768.0f) close = false;
    }
    CHECK(close, "wav.readback_within_1_5_lsb");
}

// WP-0149: pcm24/f32 writers + 24-bit reader (spec render-harness.md "Render output sample formats").
static void test_wav_formats() {
    using op::wav::float_to_pcm24;
    using op::wav::SampleFormat;

    // float_to_pcm24 table — mirrors the pcm16 half-even tests with scale 8388608.
    CHECK(float_to_pcm24(0.0f) == 0, "pcm24.zero");
    CHECK(float_to_pcm24(-1.0f) == -8388608, "pcm24.floor");
    CHECK(float_to_pcm24(1.0f) == 8388607 && float_to_pcm24(2.0f) == 8388607, "pcm24.saturation");
    CHECK(float_to_pcm24(std::nanf("")) == 0, "pcm24.nan_maps_zero");
    // half-to-even: scaled exactly .5 cases (x * 8388608.0f is exact for these inputs)
    CHECK(float_to_pcm24(0.5f / 8388608.0f) == 0, "pcm24.half_even_down_to_0");   // 0.5 → 0 (even)
    CHECK(float_to_pcm24(1.5f / 8388608.0f) == 2, "pcm24.half_even_up_to_2");     // 1.5 → 2 (even)
    CHECK(float_to_pcm24(2.5f / 8388608.0f) == 2, "pcm24.half_even_down_to_2");   // 2.5 → 2 (even)
    CHECK(float_to_pcm24(-0.5f / 8388608.0f) == 0 && float_to_pcm24(-1.5f / 8388608.0f) == -2,
          "pcm24.negative_ties");                                                  // −0.5 → 0, −1.5 → −2 (even)

    op::wav::StereoBuffer buf;
    buf.sample_rate = 48000;
    const float vals[] = {0.0f, -1.0f, 0.9999f, 2.5f, -3.0f, 0.5f / 8388608.0f};
    for (float v : vals) buf.interleaved.push_back(v), buf.interleaved.push_back(-v);
    const size_t frames = buf.frames();
    std::string err;

    auto file_bytes = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    auto rd32 = [](const std::vector<uint8_t>& v, size_t o) {
        return static_cast<uint32_t>(v[o]) | (static_cast<uint32_t>(v[o + 1]) << 8) |
               (static_cast<uint32_t>(v[o + 2]) << 16) | (static_cast<uint32_t>(v[o + 3]) << 24);
    };

    // pcm24: 44-byte header, tag 1 / bits 24 / align 6 / byte rate SR*6, data F*6, RIFF 36+F*6.
    CHECK(op::wav::write_wav_stereo(tmp_file("op_fmt_p24.wav"), buf, SampleFormat::Pcm24, err), "fmt.pcm24_write_ok");
    {
        const auto h = file_bytes(tmp_file("op_fmt_p24.wav"));
        bool ok = h.size() == 44 + frames * 6 && std::memcmp(h.data(), "RIFF", 4) == 0 &&
                  std::memcmp(h.data() + 8, "WAVE", 4) == 0 && rd32(h, 16) == 16 &&
                  (h[20] | (h[21] << 8)) == 1 && h[34] == 24 && (h[32] | (h[33] << 8)) == 6 &&
                  rd32(h, 28) == 48000u * 6u && rd32(h, 4) == 36u + static_cast<uint32_t>(frames * 6) &&
                  std::memcmp(h.data() + 36, "data", 4) == 0 && rd32(h, 40) == frames * 6;
        CHECK(ok, "fmt.pcm24_header_bytes_exact");
    }
    auto r24 = op::wav::read_wav(tmp_file("op_fmt_p24.wav"));
    bool ok24 = r24.ok && r24.samples.size() == frames * 2;
    for (size_t i = 0; ok24 && i < r24.samples.size(); ++i) {
        const float expect = static_cast<float>(float_to_pcm24(buf.interleaved[i])) * (1.0f / 8388608.0f);
        if (r24.samples[i] != expect) ok24 = false;
    }
    CHECK(ok24, "fmt.pcm24_roundtrip_exact");

    // f32: 58-byte header per the spec offset table; samples bit-for-bit, no clipping.
    CHECK(op::wav::write_wav_stereo(tmp_file("op_fmt_f32.wav"), buf, SampleFormat::F32, err), "fmt.f32_write_ok");
    {
        const auto h = file_bytes(tmp_file("op_fmt_f32.wav"));
        bool ok = h.size() == 58 + frames * 8 && std::memcmp(h.data(), "RIFF", 4) == 0 &&
                  std::memcmp(h.data() + 8, "WAVE", 4) == 0 && rd32(h, 4) == 50u + static_cast<uint32_t>(frames * 8) &&
                  rd32(h, 16) == 18 && (h[20] | (h[21] << 8)) == 3 && h[22] == 2 && rd32(h, 24) == 48000u &&
                  rd32(h, 28) == 48000u * 8u && (h[32] | (h[33] << 8)) == 8 && (h[34] | (h[35] << 8)) == 32 &&
                  (h[36] | (h[37] << 8)) == 0 && std::memcmp(h.data() + 38, "fact", 4) == 0 && rd32(h, 42) == 4 &&
                  rd32(h, 46) == frames && std::memcmp(h.data() + 50, "data", 4) == 0 && rd32(h, 54) == frames * 8;
        CHECK(ok, "fmt.f32_header_bytes_exact");
    }
    auto rf = op::wav::read_wav(tmp_file("op_fmt_f32.wav"));
    bool okf = rf.ok && rf.samples.size() == frames * 2;
    for (size_t i = 0; okf && i < rf.samples.size(); ++i) {
        uint32_t a, b;
        std::memcpy(&a, &buf.interleaved[i], sizeof(a));
        std::memcpy(&b, &rf.samples[i], sizeof(b));
        if (a != b) okf = false;  // bit-exact: 2.5 and −3.0 survive unclipped
    }
    CHECK(okf, "fmt.f32_roundtrip_bitexact_no_clipping");

    // write_pcm16_stereo ≡ write_wav_stereo(Pcm16): byte-for-byte identical files.
    bool w1 = op::wav::write_wav_stereo(tmp_file("op_fmt_p16_new.wav"), buf, SampleFormat::Pcm16, err);
    bool w2 = op::wav::write_pcm16_stereo(tmp_file("op_fmt_p16_old.wav"), buf, err);
    CHECK(w1 && w2 && file_bytes(tmp_file("op_fmt_p16_new.wav")) == file_bytes(tmp_file("op_fmt_p16_old.wav")),
          "fmt.pcm16_wrapper_byte_identical");
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

// --- synth voice (WP-0103a): MIDI pitch → frequency, octave math, fallback, validation.
static void test_synth() {
    std::string err;
    op::Project p;
    p.bpm = 60.0; p.length_bars = 1.0; p.sample_rate = 48000;
    op::Track t; t.name = "Syn";
    t.voice.kind = op::VoiceKind::Synth;  // defaults: attack 5ms, release 30ms
    p.tracks = {t};
    op::Pattern pat; pat.track = 0;
    pat.notes.push_back({0.0, 1.0, 69});  // A4 for exactly one beat = 48000 samples @60bpm
    p.patterns = {pat};

    auto freq_estimate = [](const op::wav::StereoBuffer& b) {
        long long zc = 0;
        for (long long i = 4800; i + 1 < 46000; ++i) {  // envelope edges excluded
            float a = b.interleaved[i * 2], c = b.interleaved[(i + 1) * 2];
            if ((a < 0.0f && c >= 0.0f) || (a >= 0.0f && c < 0.0f)) ++zc;
        }
        return static_cast<double>(zc) / 2.0 * 48000.0 / 41200.0;
    };

    op::wav::StereoBuffer b;
    CHECK(op::render_project(p, "", b, err), "synth.render_ok");
    double d440 = freq_estimate(b) - 440.0;
    CHECK(d440 < 2.0 && d440 > -2.0, "synth.a4_is_440hz");

    op::Project p2 = p; p2.patterns[0].notes[0].midi_note = 81;  // A5
    op::wav::StereoBuffer b2;
    CHECK(op::render_project(p2, "", b2, err), "synth.a5_render_ok");
    double ratio = freq_estimate(b2) / freq_estimate(b);
    CHECK(ratio > 1.99 && ratio < 2.01, "synth.octave_is_double_freq");

    op::Project p3 = p; p3.patterns[0].notes[0].midi_note = -1;  // absent → voice.freq_hz fallback
    op::wav::StereoBuffer b3;
    CHECK(op::render_project(p3, "", b3, err), "synth.fallback_render_ok");
    double dfb = freq_estimate(b3) - 440.0;
    CHECK(dfb < 2.0 && dfb > -2.0, "synth.absent_note_uses_freq_hz");

    write_text(tmp_file("op_test_syn_note.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"synth"}}],"patterns":[{"track":0,"notes":[{"t":0,"dur":1,"note":128}]}]})");
    op::Project pb;
    CHECK(!op::load_project(tmp_file("op_test_syn_note.opj"), pb, err), "model.rejects_midi_note_128");

    op::Project ps = p;
    CHECK(op::save_project(tmp_file("op_test_synth_rt.opj"), ps, err), "synth.save_ok");
    op::Project pl;
    CHECK(op::load_project(tmp_file("op_test_synth_rt.opj"), pl, err) &&
          pl.tracks[0].voice.kind == op::VoiceKind::Synth &&
          pl.patterns[0].notes[0].midi_note == 69, "synth.roundtrip_preserves_pitch");
}

// --- MIDI import (WP-0103b): fixture parse, conventions, fuzz-lite truncation.
static void test_midi() {
#ifndef OP_TEST_DATA
#define OP_TEST_DATA "."
#endif
    const std::string fix = std::string(OP_TEST_DATA) + "/fixtures/midi/two_notes.mid";
    std::string err;
    op::Project p;
    CHECK(op::load_midi(fix, p, err), "midi.load_fixture");
    CHECK(p.tracks.size() == 2 && p.patterns.size() == 2, "midi.two_tracks");
    CHECK(p.bpm == 120.0, "midi.tempo_from_meta");
    CHECK(p.tracks[0].name == "Lead" && p.tracks[1].name == "Bass", "midi.track_names");
    CHECK(p.tracks[0].voice.kind == op::VoiceKind::Synth, "midi.imported_voice_is_synth");
    if (p.patterns.size() == 2) {
        const auto& lead = p.patterns[0].notes;
        CHECK(lead.size() == 4 && lead[0].midi_note == 60 && lead[3].midi_note == 72, "midi.lead_notes");
        CHECK(lead[0].t_beats == 0.0 && lead[1].t_beats == 1.0 && lead[0].dur_beats == 1.0, "midi.beat_mapping_ppq480");
        const auto& bass = p.patterns[1].notes;
        CHECK(bass.size() == 1 && bass[0].midi_note == 36 && bass[0].dur_beats == 4.0, "midi.vel0_is_note_off");
    } else {
        ++g_failures; std::printf("FAIL midi.pattern_count (missing)\n");
    }
    CHECK(p.length_bars == 1.0, "midi.length_from_extent");

    // truncation fuzz: no prefix may crash or claim success
    std::ifstream f(fix, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    bool fuzz_ok = true;
    for (size_t cut = 0; cut < bytes.size() && fuzz_ok; ++cut) {
        std::string tmp = tmp_file("op_fuzz.mid");
        { std::ofstream w(tmp, std::ios::binary); w.write(reinterpret_cast<const char*>(bytes.data()), cut); }
        op::Project fp;
        if (op::load_midi(tmp, fp, err)) fuzz_ok = false;  // a truncated file must never "succeed"
    }
    CHECK(fuzz_ok, "midi.truncation_fuzz_no_crash_no_success");

    // mutation fuzz: seeded byte flips must never crash/hang; if a mutant still parses, model
    // invariants (dur > 0, pitch range) must hold — the parser's contract under garbage input.
    op::rng::Pcg32 prng(20261118ull);
    bool mut_ok = true;
    size_t mutants_parsed = 0;
    for (int trial = 0; trial < 500 && mut_ok; ++trial) {
        std::vector<uint8_t> m = bytes;
        const int nmut = 1 + static_cast<int>(prng.next() % 4u);
        for (int k = 0; k < nmut; ++k) {
            const size_t p = prng.next() % m.size();
            m[p] ^= static_cast<uint8_t>(1u << (prng.next() % 8u));
        }
        { std::ofstream w(tmp_file("op_mut.mid"), std::ios::binary); w.write(reinterpret_cast<const char*>(m.data()), static_cast<std::streamsize>(m.size())); }
        op::Project mp;
        if (op::load_midi(tmp_file("op_mut.mid"), mp, err)) {
            ++mutants_parsed;
            for (const auto& pat : mp.patterns)
                for (const auto& nn : pat.notes)
                    if (!(nn.dur_beats > 0.0) || nn.midi_note < 0 || nn.midi_note > 127 || !(mp.bpm > 0.0)) mut_ok = false;
        }
    }
    CHECK(mut_ok, "midi.mutation_fuzz_no_crash_invariants_hold");
    std::printf("INFO midi mutation fuzz: %zu/500 mutants parsed\n", mutants_parsed);

    // wrong magic + SMPTE division rejected with actionable errors
    write_text(tmp_file("op_badmagic.mid"), "RIFFjunkjunk");
    op::Project bp;
    CHECK(!op::load_midi(tmp_file("op_badmagic.mid"), bp, err), "midi.rejects_bad_magic");

    // --- export (WP-0104): byte-deterministic + exact content round-trip
    op::Project src;
    CHECK(op::load_midi(fix, src, err), "midi.rt_source_load");
    CHECK(op::save_midi(tmp_file("op_rt_a.mid"), src, err), "midi.export_ok");
    auto slurp = [](const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    CHECK(op::save_midi(tmp_file("op_rt_b.mid"), src, err), "midi.export2_ok");
    CHECK(slurp(tmp_file("op_rt_a.mid")) == slurp(tmp_file("op_rt_b.mid")), "midi.export_byte_deterministic");
    op::Project back;
    CHECK(op::load_midi(tmp_file("op_rt_a.mid"), back, err), "midi.reimport_ok");
    auto notes_of = [](const op::Project& q) {
        std::vector<std::pair<double, int>> v;
        for (const auto& pat : q.patterns)
            for (const auto& n : pat.notes) v.push_back({n.t_beats, n.midi_note});
        return v;
    };
    CHECK(notes_of(src) == notes_of(back), "midi.roundtrip_notes_exact");
    CHECK(back.bpm == src.bpm && back.tracks.size() == src.tracks.size(), "midi.roundtrip_bpm_tracks");

    // legacy multi-pattern project exports all patterns per track (gather by track field)
    op::Project lp;
    CHECK(op::load_project(std::string(OP_TEST_DATA) + "/fixtures/goldens/demo.opj", lp, err), "midi.export_legacy_load");
    CHECK(op::save_midi(tmp_file("op_legacy.mid"), lp, err), "midi.export_legacy_ok");
    {
        size_t n_notes = 0;
        op::Project lb;
        if (op::load_midi(tmp_file("op_legacy.mid"), lb, err))
            for (const auto& pat : lb.patterns) n_notes += pat.notes.size();
        size_t want_notes = 0;
        for (const auto& pat : lp.patterns) want_notes += pat.notes.size();
        CHECK(n_notes == want_notes && n_notes > 0, "midi.export_legacy_all_patterns");
    }
}

// --- tempo map (WP-0105): legacy bit-exactness, displacement across changes, validation.
static void test_tempo_map() {
    std::string err;
    op::Project a;
    a.bpm = 120.0; a.length_bars = 2.0; a.sample_rate = 48000;
    op::Track t; t.name = "S"; t.voice.kind = op::VoiceKind::Sine; t.voice.freq_hz = 220.0f;
    a.tracks = {t};
    op::Pattern pat; pat.track = 0;
    pat.notes.push_back({0.0, 0.5, -1}); pat.notes.push_back({1.0, 0.5, -1}); pat.notes.push_back({4.5, 0.5, -1});
    a.patterns = {pat};

    op::wav::StereoBuffer ba, bb;
    CHECK(op::render_project(a, "", ba, err), "tempo.legacy_render_ok");
    op::Project b = a;
    b.tempo_map.push_back({0.0, 120.0});
    CHECK(op::render_project(b, "", bb, err), "tempo.constant_map_render_ok");
    CHECK(ba.interleaved == bb.interleaved, "tempo.single_point_map_bit_identical_to_legacy");

    // Half-tempo from bar 4 (beat 16): note at beat 25 must land at 384000 + 9*48000 = 816000.
    op::Project c = a;
    c.length_bars = 8.0;
    c.tempo_map.push_back({0.0, 120.0});
    c.tempo_map.push_back({4.0, 60.0});
    c.patterns[0].notes.push_back({25.0, 0.5, -1});
    op::wav::StereoBuffer bc;
    CHECK(op::render_project(c, "", bc, err), "tempo.change_render_ok");
    const long long expect_note = 384000 + 9 * 48000;
    auto energy_in = [](const op::wav::StereoBuffer& buf, long long lo, long long hi) {
        const long long n = static_cast<long long>(buf.interleaved.size() / 2);
        if (lo < 0) lo = 0;
        if (hi > n) hi = n;  // windows may exceed shorter buffers — clamp, never overrun
        double s = 0.0;
        for (long long i = lo; i < hi; ++i) { const double x = buf.interleaved[static_cast<size_t>(i) * 2]; s += x * x; }
        return s;
    };
    CHECK(energy_in(bc, expect_note - 2000, expect_note + 4000) > 0.0, "tempo.note_after_drop_at_expected_samples");

    op::Project d = a; d.length_bars = 8.0;
    op::wav::StereoBuffer bd;
    CHECK(op::render_project(d, "", bd, err), "tempo.constant8bar_ok");
    // Stronger displacement proof in the mapped buffer itself: beat 25 at constant 120 would be
    // sample 600000 — with the drop there must be silence exactly there.
    CHECK(energy_in(bc, 600000 - 2000, 600000 + 4000) == 0.0, "tempo.note_not_at_constant_tempo_position");

    // Duration governed by local tempo at note start: 0.5 beats @60bpm = 24000 samples → silent after +30000
    CHECK(energy_in(bc, expect_note + 30000, expect_note + 100000) == 0.0, "tempo.duration_uses_local_tempo");

    write_text(tmp_file("op_tm_badbpm.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[],"tempo_map":[{"bar":0,"bpm":120},{"bar":2,"bpm":-5}]})");
    op::Project pb;
    CHECK(!op::load_project(tmp_file("op_tm_badbpm.opj"), pb, err), "model.rejects_bad_tempo_bpm");
    write_text(tmp_file("op_tm_nozero.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[],"tempo_map":[{"bar":1,"bpm":120}]})");
    CHECK(!op::load_project(tmp_file("op_tm_nozero.opj"), pb, err), "model.rejects_tempo_map_not_starting_at_zero");

    CHECK(op::save_project(tmp_file("op_tm_rt.opj"), c, err) && op::load_project(tmp_file("op_tm_rt.opj"), pb, err) &&
          pb.tempo_map.size() == 2 && pb.tempo_map[1].bpm == 60.0, "tempo.save_roundtrip");
}

// --- render a bar range (WP-0145): sample_at_bar on both tempo paths + clamps, slice_frames contents.
static void test_render_range() {
    std::string err;
    // Constant 120 bpm, 4/4 @48 kHz: spb = 24000 → bar 1 = 96000 samples (spec example).
    op::Project a;
    a.bpm = 120.0; a.length_bars = 2.0; a.sample_rate = 48000;
    op::Track t; t.name = "S"; t.voice.kind = op::VoiceKind::Sine; t.voice.freq_hz = 220.0f;
    a.tracks = {t};
    CHECK(op::sample_at_bar(a, 1.0) == 96000, "range.bar1_120bpm_is_96000");
    CHECK(op::sample_at_bar(a, 0.5) == 48000 && op::sample_at_bar(a, 1.5) == 144000, "range.fractional_bars");
    CHECK(op::sample_at_bar(a, a.length_bars) == op::total_samples_for(a), "range.bar_at_length_is_total");
    CHECK(op::sample_at_bar(a, 99.0) == op::total_samples_for(a), "range.bar_past_end_clamps_to_total");

    // tempo.opj fixture (120→72 bpm at bar 4): bar 4 = llround(16*24000) = 384000;
    // bar 5 = 384000 + llround(4 * 48000*60/72) = 384000 + 160000 = 544000 (spec example).
    op::Project tp;
    CHECK(op::load_project(std::string(OP_TEST_DATA) + "/fixtures/goldens/tempo.opj", tp, err), "range.tempo_fixture_loads");
    CHECK(op::sample_at_bar(tp, 4.0) == 384000, "range.tempo_bar4_is_384000");
    CHECK(op::sample_at_bar(tp, 5.0) == 544000, "range.tempo_bar5_is_544000");

    // slice_frames: contents + sample_rate copy; full-range slice is the identity.
    op::wav::StereoBuffer src;
    src.sample_rate = 44100;
    for (int i = 0; i < 10; ++i) { src.interleaved.push_back(static_cast<float>(i)); src.interleaved.push_back(-static_cast<float>(i)); }
    op::wav::StereoBuffer sl;
    op::slice_frames(src, 2, 5, sl);
    CHECK(sl.sample_rate == 44100 && sl.frames() == 3, "range.slice_shape");
    const float want[6] = {2.f, -2.f, 3.f, -3.f, 4.f, -4.f};
    CHECK(std::equal(sl.interleaved.begin(), sl.interleaved.end(), want), "range.slice_contents");
    op::wav::StereoBuffer full;
    op::slice_frames(src, 0, static_cast<long long>(src.frames()), full);
    CHECK(full.interleaved == src.interleaved && full.sample_rate == src.sample_rate, "range.slice_full_range_identity");
}

// --- click track render (WP-0148): onsets via the renderer's own beat→sample mapping, bursts per spec.
static void test_click() {
    // Demo-like constant tempo: 120 bpm @ 48 kHz → 24000 samples/beat; 4 bars = 16 beats (at(b) < total).
    op::Project p;
    p.bpm = 120.0; p.length_bars = 4.0; p.beats_per_bar = 4; p.sample_rate = 48000;
    const std::vector<long long> on = op::beat_onsets(p);
    bool seq_ok = on.size() == 16;
    for (size_t b = 0; b < on.size() && seq_ok; ++b)
        seq_ok = on[b] == static_cast<long long>(b) * 24000;
    CHECK(seq_ok, "click.onsets_constant_tempo_0_24000_48000");

    // Tempo map (tempo.opj shape): 120 bpm to bar 4, then 72 bpm → beat 16 @ 384000, beat 17 @ 424000.
    op::Project tm;
    tm.bpm = 120.0; tm.length_bars = 8.0; tm.beats_per_bar = 4; tm.sample_rate = 48000;
    tm.tempo_map = {{0.0, 120.0}, {4.0, 72.0}};
    const std::vector<long long> on2 = op::beat_onsets(tm);
    CHECK(on2.size() > 17 && on2[16] == 384000 && on2[17] == 424000, "click.onsets_tempo_map_beat16_17");

    op::wav::StereoBuffer a, b;
    std::string err;
    CHECK(op::render_click(p, a, err), "click.render_ok");
    CHECK(static_cast<long long>(a.frames()) == op::total_samples_for(p), "click.length_matches_total_samples");
    // Onset sample of the first burst (beat 0 = downbeat) is exactly 0.5 in both channels.
    CHECK(a.interleaved[0] == 0.5f && a.interleaved[1] == 0.5f, "click.onset_sample_is_half_both_channels");

    // Downbeat burst (1500 Hz) differs from a plain beat burst (1000 Hz) at n = 1.
    const float down_n1 = a.interleaved[2];              // burst @ onset 0, sample n=1 (downbeat)
    const float beat_n1 = a.interleaved[(24000 + 1) * 2];  // burst @ beat 1, sample n=1
    CHECK(down_n1 != beat_n1, "click.downbeat_differs_from_beat_at_n1");

    // Everything between bursts is exactly zero (both channels).
    const long long L = static_cast<long long>(48000) * 30 / 1000;
    bool gap_zero = true;
    for (long long i = L; i < 24000 && gap_zero; ++i)
        gap_zero = a.interleaved[i * 2] == 0.0f && a.interleaved[i * 2 + 1] == 0.0f;
    CHECK(gap_zero, "click.samples_outside_bursts_exactly_zero");

    // Determinism: two calls produce bit-identical buffers.
    CHECK(op::render_click(p, b, err), "click.rerender_ok");
    CHECK(a.interleaved == b.interleaved, "click.determinism_bit_identical_rerun");
}

// --- .opd bundles (WP-0110): deterministic pack, verified unpack, render equivalence, guards.
static void test_bundle() {
    std::string err;
    // Source project with a sampler asset so the bundle actually has to carry files.
    std::error_code sec;
    std::filesystem::remove_all(tmp_file("op_bd"), sec);
    std::filesystem::create_directories(tmp_file("op_bd/kits"), sec);
    const std::string dir = tmp_file("op_bd");
    {   // 0.1s 220Hz tone as fake kick (float32 stereo → engine wav writer handles conversion)
        op::wav::StereoBuffer sb;
        sb.sample_rate = 48000;
        sb.interleaved.assign(4800 * 2, 0.0f);
        for (int i = 0; i < 4800; ++i) {
            const float s = op::dsp::sinf_(0.01f * static_cast<float>(i)) * 0.5f;
            sb.interleaved[i * 2] = s; sb.interleaved[i * 2 + 1] = s;
        }
        CHECK(op::wav::write_pcm16_stereo(dir + "/kits/kick.wav", sb, err), "bundle.asset_written");
    }
    op::Project p;
    p.bpm = 130.0; p.length_bars = 2.0; p.sample_rate = 48000; p.seed = 9;
    op::Track tk; tk.name = "Kick"; tk.voice.kind = op::VoiceKind::Sampler; tk.voice.path = "kits/kick.wav";
    op::Track tn; tn.name = "Sine"; tn.voice.kind = op::VoiceKind::Sine; tn.voice.freq_hz = 330.0f;
    p.tracks = {tk, tn};
    op::Pattern pk; pk.track = 0;
    for (int i = 0; i < 8; ++i) pk.notes.push_back({static_cast<double>(i) * 0.5, 0.25, -1});
    op::Pattern pn; pn.track = 1; pn.notes.push_back({0.0, 3.0, -1});
    p.patterns = {pk, pn};
    CHECK(op::save_project(dir + "/song.opj", p, err), "bundle.project_saved");

    // Deterministic bytes: packing twice yields identical files.
    CHECK(op::pack_project(dir + "/song.opj", dir + "/a.opd", err), "bundle.pack_ok");
    // list_bundle: metadata view matches archive contents in order.
    {
        std::vector<std::string> entries;
        CHECK(op::list_bundle(dir + "/a.opd", entries, err) && entries.size() == 2 &&
              entries[0] == "song.opj" && entries[1] == "kits/kick.wav", "bundle.list_entries");
    }
    CHECK(op::pack_project(dir + "/song.opj", dir + "/b.opd", err), "bundle.pack2_ok");
    auto slurp = [](const std::string& pth) {
        std::ifstream f(pth, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    CHECK(slurp(dir + "/a.opd") == slurp(dir + "/b.opd"), "bundle.pack_byte_deterministic");

    // Unpack → load → render must equal the original render bit-for-bit (sampler path resolved).
    const std::string out_dir = dir + "/restored";
    std::string main_opj;
    CHECK(op::unpack_bundle(dir + "/a.opd", out_dir, main_opj, err), "bundle.unpack_ok");
    op::Project rp;
    CHECK(!main_opj.empty() && op::load_project(main_opj, rp, err), "bundle.unpacked_loads");
    op::wav::StereoBuffer b1, b2;
    CHECK(op::render_project(p, dir, b1, err) && op::render_project(rp, out_dir, b2, err), "bundle.renders_ok");
    CHECK(b1.interleaved == b2.interleaved, "bundle.render_bit_identical_after_roundtrip");

    // Corruption: flip a byte deep inside the archive → clean CRC failure.
    {
        std::vector<uint8_t> raw = slurp(dir + "/a.opd");
        CHECK(raw.size() > 64, "bundle.packed_file_nonempty");
        if (raw.size() > 64) {
            raw[raw.size() / 2] ^= 0x40;
            std::ofstream w(dir + "/corrupt.opd", std::ios::binary);
            w.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            w.close();
            std::string bad_main;
            CHECK(!op::unpack_bundle(dir + "/corrupt.opd", dir + "/bad_out", bad_main, err), "bundle.corruption_rejected");
        }
    }

    // Path traversal: hand-built zip with entry "../evil.txt" must be refused by name validation.
    {   // minimal store-only zip: one empty entry named ../evil.txt (local + CD + EOCD)
        const std::string name = "../evil.txt";
        std::vector<uint8_t> z;
        auto put16 = [](std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(static_cast<uint8_t>(x >> 8)); };
        auto put32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); };
        size_t local_off = z.size();
        z.insert(z.end(), {'P', 'K', 0x03, 0x04});
        put16(z, 20); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0x21);
        put32(z, 0); put32(z, 0); put32(z, 0);
        put16(z, static_cast<uint16_t>(name.size())); put16(z, 0);
        z.insert(z.end(), name.begin(), name.end());
        size_t cd_off = z.size();
        z.insert(z.end(), {'P', 'K', 0x01, 0x02});
        put16(z, 20); put16(z, 20); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0x21);
        put32(z, 0); put32(z, 0); put32(z, 0);
        put16(z, static_cast<uint16_t>(name.size())); put16(z, 0); put16(z, 0); put16(z, 0); put16(z, 0); put32(z, 0);
        put32(z, static_cast<uint32_t>(local_off));
        z.insert(z.end(), name.begin(), name.end());
        z.insert(z.end(), {'P', 'K', 0x05, 0x06});
        put16(z, 0); put16(z, 0); put16(z, 1); put16(z, 1);
        put32(z, static_cast<uint32_t>(z.size() - cd_off)); put32(z, static_cast<uint32_t>(cd_off)); put16(z, 0);
        std::ofstream w(dir + "/evil.opd", std::ios::binary);
        w.write(reinterpret_cast<const char*>(z.data()), static_cast<std::streamsize>(z.size()));
        w.close();
        std::string bad_main;
        CHECK(!op::unpack_bundle(dir + "/evil.opd", dir + "/evil_out", bad_main, err), "bundle.path_traversal_rejected");
    }
}

// --- velocity (WP-0111): parse/save rules, exact gain behavior, MIDI capture.
static void test_velocity() {
    std::string err;
    // Parse: default + explicit + rejections.
    write_text(tmp_file("op_vel.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1,"vel":0.5},{"t":2,"dur":1}]}]})");
    op::Project p;
    CHECK(op::load_project(tmp_file("op_vel.opj"), p, err) && p.patterns[0].notes.size() == 2 &&
          p.patterns[0].notes[0].velocity == 0.5 && p.patterns[0].notes[1].velocity == 1.0,
          "vel.parse_default_and_explicit");
    write_text(tmp_file("op_vel0.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1,"vel":0.0}]}]})");
    op::Project pb;
    CHECK(!op::load_project(tmp_file("op_vel0.opj"), pb, err), "model.rejects_vel_zero");
    write_text(tmp_file("op_vel2.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1,"vel":1.5}]}]})");
    CHECK(!op::load_project(tmp_file("op_vel2.opj"), pb, err), "model.rejects_vel_above_one");

    // Save: velocity omitted when default → legacy canonical bytes unchanged.
    op::Project q;
    q.bpm = 120.0; q.length_bars = 1.0; q.sample_rate = 48000;
    op::Track t; t.name = "S"; t.voice.kind = op::VoiceKind::Sine; t.voice.freq_hz = 220.0f;
    q.tracks = {t};
    op::Pattern pat; pat.track = 0; pat.notes.push_back({0.0, 1.0, -1});
    q.patterns = {pat};
    CHECK(op::save_project(tmp_file("op_vel_save.opj"), q, err), "vel.save_ok");
    {
        std::ifstream f(tmp_file("op_vel_save.opj"));
        const std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(txt.find("\"vel\"") == std::string::npos, "vel.omitted_from_save_when_default");
    }

    // Render: vel 1.0 (no field) vs parsed project identical bit-for-bit; vel 0.5 halves peak exactly.
    op::wav::StereoBuffer b1, bh;
    CHECK(op::render_project(q, "", b1, err), "vel.render_default_ok");
    CHECK(op::load_project(tmp_file("op_vel.opj"), p, err) && p.sample_rate == 48000, "vel.reload_ok");
    auto peak = [](const op::wav::StereoBuffer& b) {
        float m = 0.0f;
        for (float x : b.interleaved) { const float a = x < 0 ? -x : x; if (a > m) m = a; }
        return m;
    };
    // half-gain twin of the default project:
    op::Project qh = q; qh.patterns[0].notes[0].velocity = 0.5;
    CHECK(op::render_project(qh, "", bh, err), "vel.render_half_ok");
    const float pk1 = peak(b1), pkh = peak(bh);
    const float rel = std::fabs(pk1 - 2.0f * pkh) / pk1;
    CHECK(rel < 1e-6f, "vel.half_velocity_halves_peak");

    // MIDI import captures velocity (fixture uses vel 90): every imported note is 90/127.
    const std::string fix = std::string(OP_TEST_DATA) + "/fixtures/midi/two_notes.mid";
    op::Project mp;
    CHECK(op::load_midi(fix, mp, err), "vel.midi_import_load");
    bool all90 = true;
    for (const auto& pt : mp.patterns)
        for (const auto& n : pt.notes)
            if (std::fabs(n.velocity - 90.0 / 127.0) > 1e-12) all90 = false;
    CHECK(all90, "vel.midi_import_captures_velocity");
    // Export → reimport: velocity survives within one MIDI step (±0.5/127).
    CHECK(op::save_midi(tmp_file("op_vel.mid"), mp, err), "vel.midi_export_ok");
    op::Project mb;
    CHECK(op::load_midi(tmp_file("op_vel.mid"), mb, err), "vel.midi_reimport_ok");
    bool vel_rt = mb.patterns.size() == mp.patterns.size();
    for (size_t i = 0; vel_rt && i < mb.patterns.size(); ++i) {
        const auto& a = mp.patterns[i].notes; const auto& b2v = mb.patterns[i].notes;
        if (a.size() != b2v.size()) { vel_rt = false; break; }
        for (size_t k = 0; k < a.size(); ++k)
            if (std::fabs(a[k].velocity - b2v[k].velocity) > 0.5 / 127.0 + 1e-12) vel_rt = false;
    }
    CHECK(vel_rt, "vel.midi_roundtrip_preserves_velocity");
}

// --- MIDI merge into existing project (WP-0114).
static void test_midi_merge() {
    const std::string fix = std::string(OP_TEST_DATA) + "/fixtures/midi/two_notes.mid";
    std::string err;

    op::Project host;
    host.bpm = 120.0; host.length_bars = 8.0; host.sample_rate = 48000;
    op::Track th; th.name = "Host"; th.voice.kind = op::VoiceKind::Sine; th.voice.freq_hz = 110.0f;
    host.tracks = {th};
    op::Pattern ph; ph.track = 0; ph.notes.push_back({0.0, 7.0, -1});
    host.patterns = {ph};

    CHECK(op::import_midi_into(host, fix, err), "merge.ok");
    CHECK(host.tracks.size() == 3 && host.patterns.size() == 3, "merge.tracks_appended");
    CHECK(host.patterns[1].track == 1 && host.patterns[2].track == 2, "merge.indices_sequential");
    // Grid semantics: positions identical to standalone import (no rescale).
    op::Project alone;
    CHECK(op::load_midi(fix, alone, err), "merge.reference_load");
    bool grid_same = alone.patterns.size() == 2 &&
                     host.patterns[1].notes.size() == alone.patterns[0].notes.size() &&
                     host.patterns[2].notes.size() == alone.patterns[1].notes.size();
    for (size_t k = 0; grid_same && k < alone.patterns[0].notes.size(); ++k)
        if (host.patterns[1].notes[k].t_beats != alone.patterns[0].notes[k].t_beats ||
            host.patterns[1].notes[k].midi_note != alone.patterns[0].notes[k].midi_note) grid_same = false;
    CHECK(grid_same, "merge.grid_semantics_exact");
    CHECK(host.tracks[1].voice.kind == op::VoiceKind::Synth, "merge.imported_voice_synth");
    CHECK(host.length_bars == 8.0, "merge.length_not_shrunk");

    // Length extends when content exceeds the host (fixture ends at beat 4 → 0.25-bar host → 1 bar).
    op::Project tiny = host;
    tiny.tracks = {th}; tiny.patterns = {ph}; tiny.length_bars = 0.25;
    CHECK(op::import_midi_into(tiny, fix, err) && tiny.length_bars == 1.0, "merge.length_extends");

    // Failure leaves host untouched: missing file → error + unchanged structure.
    op::Project before = host;
    CHECK(!op::import_midi_into(host, tmp_file("definitely_missing_file.mid"), err), "merge.missing_fails");
    CHECK(host.tracks.size() == before.tracks.size() && host.patterns.size() == before.patterns.size(),
          "merge.failure_leaves_project_untouched");

    // Render works with merged content (non-silent, valid).
    op::wav::StereoBuffer b;
    CHECK(op::render_project(host, "", b, err), "merge.render_ok");
    double e = 0.0;
    for (size_t i = 0; i < b.interleaved.size(); i += 2) e += b.interleaved[i] * b.interleaved[i];
    CHECK(e > 0.0, "merge.render_has_audio");
}

// --- tempo map ⇄ MIDI (WP-0115): multi-point export, single-tempo byte stability, round-trip.
static void test_tempo_midi() {
    std::string err;
    op::Project p;
    p.bpm = 120.0; p.length_bars = 8.0; p.sample_rate = 48000;
    op::Track t; t.name = "S"; t.voice.kind = op::VoiceKind::Synth; t.voice.freq_hz = 220.0f;
    p.tracks = {t};
    op::Pattern pat; pat.track = 0; pat.notes.push_back({0.0, 2.0, 60}); pat.notes.push_back({18.0, 2.0, 64});
    p.patterns = {pat};
    p.tempo_map = {{0.0, 120.0}, {4.0, 72.0}};

    // Export: two FF51 events, second at tick 4·4·480 with us-per-quarter ≈ 6e7/72.
    CHECK(op::save_midi(tmp_file("op_tm.mid"), p, err), "tmid.export_ok");
    std::ifstream f(tmp_file("op_tm.mid"), std::ios::binary);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<long long> tempo_ticks;
    std::vector<int> tempo_bpm;
    {   // walk track 0 body (header 14 + MTrk 8): events with running VLQ deltas
        size_t i = 22; long long tick = 0;
        while (i + 4 < raw.size()) {
            long long delta = 0;
            while (i < raw.size()) { const uint8_t b = raw[i++]; delta = (delta << 7) | (b & 0x7F); if (!(b & 0x80)) break; }
            tick += delta;
            if (raw[i] != 0xFF) break;  // first note ends the meta prefix we care about
            const uint8_t mt = raw[i + 1];
            const uint32_t len = raw[i + 2];
            if (mt == 0x51) {
                const uint32_t us = (raw[i + 3] << 16) | (raw[i + 4] << 8) | raw[i + 5];
                tempo_ticks.push_back(tick);
                tempo_bpm.push_back(static_cast<int>(std::llround(6.0e7 / static_cast<double>(us))));
            }
            if (mt == 0x2F) break;
            i += 3 + len;
        }
    }
    CHECK(tempo_ticks.size() == 2 && tempo_ticks[1] == 4LL * 4 * 480, "tmid.two_ff51");
    CHECK(tempo_bpm.size() == 2 && tempo_bpm[0] == 120 && tempo_bpm[1] == 72, "tmid.tempo_values");

    // Round-trip: import restores a two-point map (bar exact; bpm within µs-quantization tolerance).
    op::Project q;
    CHECK(op::load_midi(tmp_file("op_tm.mid"), q, err), "tmid.reimport_ok");
    CHECK(q.tempo_map.size() == 2 && q.tempo_map[1].bar == 4.0 &&
          std::fabs(q.tempo_map[1].bpm - 72.0) < 1e-3, "tmid.map_restored");

    // Single-tempo byte stability: empty map and {0,bpm} export identically.
    op::Project e = p; e.tempo_map.clear();
    op::Project s = p; s.tempo_map = {{0.0, 120.0}};
    CHECK(op::save_midi(tmp_file("op_tm_empty.mid"), e, err) && op::save_midi(tmp_file("op_tm_single.mid"), s, err), "tmid.singles_save");
    std::ifstream f1(tmp_file("op_tm_empty.mid"), std::ios::binary), f2(tmp_file("op_tm_single.mid"), std::ios::binary);
    const std::vector<uint8_t> b1((std::istreambuf_iterator<char>(f1)), std::istreambuf_iterator<char>());
    const std::vector<uint8_t> b2((std::istreambuf_iterator<char>(f2)), std::istreambuf_iterator<char>());
    CHECK(b1 == b2, "tmid.empty_and_single_point_identical");

    // Legacy guard: single-tempo fixture imports with EMPTY map (imported.opj golden bytes safe).
    const std::string fix = std::string(OP_TEST_DATA) + "/fixtures/midi/two_notes.mid";
    op::Project leg;
    CHECK(op::load_midi(fix, leg, err) && leg.tempo_map.empty(), "tmid.single_tempo_imports_legacy");
}

// --- attachments (WP-0116): parse/save rules, add-op validation, bundle carrying.
static void test_attachments() {
    std::string err;
    // Parse + reject.
    write_text(tmp_file("op_att.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[],"attachments":[{"path":"stems/drums.wav","kind":"stem","producer":"demucs"},
                                      {"path":"hum.mid","kind":"transcription"}]})");
    op::Project p;
    CHECK(op::load_project(tmp_file("op_att.opj"), p, err) && p.attachments.size() == 2 &&
          p.attachments[0].producer == "demucs" && p.attachments[1].producer.empty(),
          "att.parse_two");
    const char* bad[] = {
        R"([{"path":"/abs/x.wav","kind":"stem"}])",
        R"([{"path":"../escape.wav","kind":"stem"}])",
        R"([{"path":"ok.wav","kind":"magic"}])",
        R"([{"path":"","kind":"stem"}])"};
    for (const char* body : bad) {
        write_text(tmp_file("op_att_bad.opj"),
                   std::string(R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[],"attachments":)") +
                   body + "}");
        op::Project pb;
        CHECK(!op::load_project(tmp_file("op_att_bad.opj"), pb, err), "att.rejects_bad_input");
    }

    // add_attachment op: ok / dup / invalid-kind.
    op::Attachment a{"stems/bass.wav", "stem", "demucs ht"};
    CHECK(op::add_attachment(p, a, err) && p.attachments.size() == 3, "att.add_ok");
    CHECK(!op::add_attachment(p, a, err), "att.reject_duplicate");
    CHECK(!op::add_attachment(p, {"x.wav", "hologram", ""}, err), "att.reject_bad_kind");

    // Save omits attachments when empty; preserves them otherwise (round-trip stable).
    op::Project q = p;
    CHECK(op::save_project(tmp_file("op_att_rt.opj"), q, err), "att.save_ok");
    op::Project r;
    bool same = op::load_project(tmp_file("op_att_rt.opj"), r, err) && r.attachments.size() == 3 &&
                r.attachments[2].path == "stems/bass.wav" && r.attachments[2].producer == "demucs ht";
    CHECK(same, "att.roundtrip");

    // Bundle carries existing attachments (deduped with samplers); missing ones are skipped.
    std::error_code sec;
    std::filesystem::remove_all(tmp_file("op_att_bd"), sec);
    std::filesystem::create_directories(tmp_file("op_att_bd/stems"), sec);
    std::filesystem::create_directories(tmp_file("op_att_bd/kits"), sec);
    auto copyfile = [](const std::string& a, const std::string& b) {
        std::ifstream src(a, std::ios::binary);
        std::ofstream dst(b, std::ios::binary);
        dst << src.rdbuf();
    };
    copyfile(tmp_file("op_bd/kits/kick.wav"), tmp_file("op_att_bd/stems/drums.wav"));  // fake stem
    copyfile(tmp_file("op_bd/kits/kick.wav"), tmp_file("op_att_bd/kits/kick.wav"));
    op::Project bp;
    bp.bpm = 120.0; bp.length_bars = 1.0; bp.sample_rate = 48000;
    op::Track tk; tk.name = "K"; tk.voice.kind = op::VoiceKind::Sampler; tk.voice.path = "kits/kick.wav";
    bp.tracks = {tk};
    CHECK(op::add_attachment(bp, {"stems/drums.wav", "stem", "demucs"}, err), "att.bundle_add");
    CHECK(op::add_attachment(bp, {"gone/missing.wav", "other", ""}, err), "att.bundle_add_missing_ref");
    CHECK(op::save_project(tmp_file("op_att_bd/song.opj"), bp, err), "att.bundle_save");
    CHECK(op::pack_project(tmp_file("op_att_bd/song.opj"), tmp_file("op_att_bd/s.opd"), err), "att.pack_ok");
    std::vector<std::string> entries;
    CHECK(op::list_bundle(tmp_file("op_att_bd/s.opd"), entries, err), "att.list_ok");
    bool has_stem = false, no_missing = true;
    for (const auto& e : entries) {
        if (e == "stems/drums.wav") has_stem = true;
        if (e.find("missing") != std::string::npos) no_missing = false;
    }
    CHECK(has_stem, "att.bundle_carries_existing");
    CHECK(no_missing, "att.bundle_skips_missing");
}

// --- clip gain (WP-0120).
static void test_clip_gain() {
    std::string err;
    // Parse default/explicit + range rejection.
    write_text(tmp_file("op_cg.opj"), R"({"format_version":0,"bpm":120,"length_bars":4,
        "tracks":[{"voice":{"kind":"sine","freq_hz":220}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":3.5}]}],
        "arrangement":[{"pattern":0,"start_bar":0},{"pattern":0,"start_bar":2,"gain_db":-6.0}]})");
    op::Project p;
    CHECK(op::load_project(tmp_file("op_cg.opj"), p, err) && p.arrangement.size() == 2 &&
          p.arrangement[0].gain_db == 0.0 && p.arrangement[1].gain_db == -6.0, "cg.parse");
    write_text(tmp_file("op_cg_bad.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}],
        "arrangement":[{"pattern":0,"gain_db":30.0}]})");
    op::Project pb;
    CHECK(!op::load_project(tmp_file("op_cg_bad.opj"), pb, err), "cg.rejects_out_of_range");

    // Save: omitted at unity, preserved otherwise (round-trip).
    op::Project q = p;
    CHECK(op::save_project(tmp_file("op_cg_rt.opj"), q, err), "cg.save_ok");
    // NOTE: search is scoped after `"arrangement"` — Track also has a gain_db field.
    auto arrangement_slice = [](const std::string& fpath) {
        std::ifstream f(fpath);
        const std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const size_t pos = txt.find("\"arrangement\"");
        if (pos == std::string::npos) return std::string();
        const size_t open_b = txt.find('[', pos);
        const size_t close_b = txt.find(']', open_b);  // clips are flat: first ']' closes the array
        return txt.substr(open_b, close_b - open_b);
    };
    CHECK(arrangement_slice(tmp_file("op_cg_rt.opj")).find("gain_db") != std::string::npos, "cg.saved_when_set");
    op::Project r;
    CHECK(op::load_project(tmp_file("op_cg_rt.opj"), r, err) && r.arrangement[1].gain_db == -6.0, "cg.roundtrip");
    op::Project unity = p;
    unity.arrangement[1].gain_db = 0.0;
    CHECK(op::save_project(tmp_file("op_cg_u.opj"), unity, err), "cg.save_unity_ok");
    CHECK(arrangement_slice(tmp_file("op_cg_u.opj")).find("gain_db") == std::string::npos, "cg.omitted_at_unity");

    // Render: half-loudness clip (≈ -6.0206 dB) halves the second window's energy vs unity twin.
    auto energy = [](const op::wav::StereoBuffer& b, long long lo, long long hi) {
        double e = 0.0;
        const long long n = static_cast<long long>(b.interleaved.size()) / 2;
        if (lo < 0) lo = 0;
        if (hi > n) hi = n;
        for (long long i = lo; i < hi; ++i) {
            e += static_cast<double>(b.interleaved[i * 2]) * b.interleaved[i * 2];
            e += static_cast<double>(b.interleaved[i * 2 + 1]) * b.interleaved[i * 2 + 1];
        }
        return e;
    };
    op::wav::StereoBuffer full_u, full_h;
    CHECK(op::render_project(unity, "", full_u, err) && op::render_project(p, "", full_h, err), "cg.renders_ok");
    // Second clip starts at bar 2 = beat 8; at 120 BPM one beat = 24000 samples → note spans
    // samples [192000, 276000); window skips the attack edge.
    const long long w0 = 194000, w1 = 224000;
    const double e_u = energy(full_u, w0, w1), e_h = energy(full_h, w0, w1);
    const double ratio = e_h / e_u;  // half amplitude → quarter energy
    CHECK(e_u > 0.0 && std::fabs(ratio - 0.25) < 0.02, "cg.half_gain_quarters_energy");
    // First window (unity clip only) must be identical between the two projects.
    CHECK(energy(full_u, 0, 90000) == energy(full_h, 0, 90000), "cg.untouched_clip_bit_identical");
}

// --- Mixer mute/solo/master gain (WP-0136): audibility bit-identity, master multiply exactness,
// stem zeros/sums, parser type+range validation, canonical-save default omission.

// 3 tracks × one pattern each, no overlaps → stems sum is bit-exact at defaults (see test_render).
static op::Project mixer_base_project() {
    op::Project p;
    p.bpm = 120.0; p.length_bars = 2.0; p.beats_per_bar = 4; p.sample_rate = 48000; p.seed = 7ull;
    op::Track a; a.name = "Metro"; a.voice.kind = op::VoiceKind::Metronome;
    op::Track b; b.name = "Sine";  b.voice.kind = op::VoiceKind::Sine;  b.voice.freq_hz = 220.0f;
    op::Track c; c.name = "Synth"; c.voice.kind = op::VoiceKind::Synth;
    p.tracks = {a, b, c};
    op::Pattern pa; pa.track = 0; pa.notes.push_back({0.0, 0.25, -1, 1.0});
    op::Pattern pb; pb.track = 1; pb.notes.push_back({0.5, 1.0, -1, 1.0});
    op::Pattern pc; pc.track = 2; pc.notes.push_back({1.0, 1.0, 60, 1.0});
    p.patterns = {pa, pb, pc};
    return p;
}

// Copy of q holding only the `keep` tracks (original relative order); pattern.track indices are
// re-pointed and patterns of dropped tracks go with them. The deletion-equivalence comparator for
// audibility bit-identity (callers keep master_gain_db at 0 dB — spec).
static op::Project mixer_kept(const op::Project& q, const std::vector<size_t>& keep) {
    op::Project r = q;
    r.tracks.clear();
    r.patterns.clear();
    std::vector<int> remap(q.tracks.size(), -1);
    for (size_t k = 0; k < keep.size(); ++k) {
        remap[keep[k]] = static_cast<int>(k);
        r.tracks.push_back(q.tracks[keep[k]]);
    }
    for (const auto& pat : q.patterns) {
        if (pat.track >= 0 && remap[static_cast<size_t>(pat.track)] >= 0) {
            op::Pattern np = pat;
            np.track = remap[static_cast<size_t>(pat.track)];
            r.patterns.push_back(std::move(np));
        }
    }
    return r;
}

static void test_mixer() {
    std::string err;
    const op::Project base = mixer_base_project();
    auto render = [&](const op::Project& q) {
        op::wav::StereoBuffer b;
        CHECK(op::render_project(q, "", b, err), "mixer.render_ok");
        return b;
    };
    // Bit-identity = byte identity (memcmp): the engine's voices can emit NaN payloads and float
    // == is false for NaN vs NaN even when the bits match — goldens compare with cmp for the same reason.
    auto bytes_equal = [](const op::wav::StereoBuffer& a, const op::wav::StereoBuffer& b) {
        return a.interleaved.size() == b.interleaved.size() &&
               std::memcmp(a.interleaved.data(), b.interleaved.data(),
                           a.interleaved.size() * sizeof(float)) == 0;
    };

    // track_audible rule table (spec §mixer): everything audible by default; solo gates the rest;
    // mute wins over solo.
    {
        op::Project q = mixer_base_project();
        CHECK(op::track_audible(q, 0) && op::track_audible(q, 1) && op::track_audible(q, 2),
              "mixer.audible_all_by_default");
        q.tracks[1].solo = true;
        CHECK(!op::track_audible(q, 0) && op::track_audible(q, 1) && !op::track_audible(q, 2),
              "mixer.solo_gates_others");
        q.tracks[1].mute = true;
        CHECK(!op::track_audible(q, 1), "mixer.mute_wins_over_solo");
    }

    // (a) Bit-identity vs deletion of inaudible tracks (master at 0 dB):
    // mute-only / solo-one / two-solos; mute+solo on one track → nothing audible → silence.
    CHECK(!bytes_equal(render(base), render(mixer_kept(base, {1}))),
          "mixer.full_mix_differs_from_partial");  // guard: the equality checks below can fail
    { op::Project q = base; q.tracks[0].mute = true;
      CHECK(bytes_equal(render(q), render(mixer_kept(base, {1, 2}))),
            "mixer.mute_only_equals_deletion"); }
    { op::Project q = base; q.tracks[1].solo = true;
      CHECK(bytes_equal(render(q), render(mixer_kept(base, {1}))),
            "mixer.solo_one_equals_deletion"); }
    { op::Project q = base; q.tracks[1].mute = true; q.tracks[1].solo = true;
      const auto got = render(q);
      const std::vector<float> zeros(got.interleaved.size(), 0.0f);
      CHECK(got.interleaved == zeros, "mixer.mute_plus_solo_is_silence"); }
    { op::Project q = base; q.tracks[1].solo = true; q.tracks[2].solo = true;
      CHECK(bytes_equal(render(q), render(mixer_kept(base, {1, 2}))),
            "mixer.solo_two_equals_deletion"); }

    // (b) Master −6 dB: every sample equals the 0 dB render × db_to_lin(−6), same float op.
    const auto dry = render(base);
    { op::Project q = base; q.master_gain_db = -6.0f;
      const auto wet = render(q);
      const float m = op::dsp::db_to_lin(-6.0f);
      op::wav::StereoBuffer want = dry;
      for (float& s : want.interleaved) s *= m;  // identical float op → NaN payloads propagate same bits
      CHECK(bytes_equal(wet, want), "mixer.master_minus6_is_exact_scale"); }

    // (c) Stems with mute + master −6: inaudible stem all zeros at full length; sum ≈ mix
    // (float-associativity only once the master multiply is live — see render.hpp).
    { op::Project q = base; q.tracks[0].mute = true; q.master_gain_db = -6.0f;
      std::vector<op::wav::StereoBuffer> stems(3);
      op::wav::StereoBuffer mix;
      CHECK(op::render_stems(q, "", stems, mix, err), "mixer.stems_ok");
      bool all_zero = stems[0].interleaved.size() == mix.interleaved.size();
      if (all_zero)
          for (float s : stems[0].interleaved)
              if (s != 0.0f) { all_zero = false; break; }
      CHECK(all_zero, "mixer.inaudible_stem_all_zeros");
      bool close = true;
      for (size_t i = 0; i < mix.interleaved.size(); ++i) {
          const float sum = stems[0].interleaved[i] + stems[1].interleaved[i] + stems[2].interleaved[i];
          if (sum != sum && mix.interleaved[i] != mix.interleaved[i]) continue;  // NaN at same spot: equal
          if (std::fabs(sum - mix.interleaved[i]) > 1e-5f) { close = false; break; }
      }
      CHECK(close, "mixer.stems_sum_approx_mix_with_master"); }

    // Parser validation: gain/pan ranges (gap closure) and mute/solo must be JSON booleans.
    op::Project q; std::string e;
    write_text(tmp_file("op_mx_gain.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"},"gain_db":99}]})");
    CHECK(!op::load_project(tmp_file("op_mx_gain.opj"), q, e) && e == "track.gain_db out of range [-96, 24]",
          "mixer.reject_track_gain_99");
    write_text(tmp_file("op_mx_pan.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"},"pan":5}]})");
    CHECK(!op::load_project(tmp_file("op_mx_pan.opj"), q, e) && e == "track.pan out of range [-1, 1]",
          "mixer.reject_track_pan_5");
    write_text(tmp_file("op_mx_master.opj"), R"({"format_version":0,"master_gain_db":30,"tracks":[{"voice":{"kind":"sine"}}]})");
    CHECK(!op::load_project(tmp_file("op_mx_master.opj"), q, e) && e == "master_gain_db out of range [-96, 24]",
          "mixer.reject_master_30");
    write_text(tmp_file("op_mx_mute.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"},"mute":1}]})");
    CHECK(!op::load_project(tmp_file("op_mx_mute.opj"), q, e) && e == "track.mute must be a boolean",
          "mixer.reject_mute_not_bool");
    write_text(tmp_file("op_mx_solo.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"},"solo":"yes"}]})");
    CHECK(!op::load_project(tmp_file("op_mx_solo.opj"), q, e) && e == "track.solo must be a boolean",
          "mixer.reject_solo_not_bool");

    // Canonical save: defaults omitted → legacy bytes unchanged; mixer fields round-trip stably.
    auto read_all = [](const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    { op::Project d = mixer_base_project();
      CHECK(op::save_project(tmp_file("op_mx_def.opj"), d, err), "mixer.save_default_ok");
      const std::string s = read_all(tmp_file("op_mx_def.opj"));
      CHECK(s.find("mute") == std::string::npos && s.find("solo") == std::string::npos &&
                s.find("master_gain_db") == std::string::npos, "mixer.defaults_omitted"); }
    { op::Project d = mixer_base_project(); d.tracks[0].mute = true; d.master_gain_db = -6.0f;
      CHECK(op::save_project(tmp_file("op_mx_mix.opj"), d, err), "mixer.save_mixer_ok");
      op::Project r;
      CHECK(op::load_project(tmp_file("op_mx_mix.opj"), r, err) && r.tracks[0].mute &&
                !r.tracks[1].solo && !r.tracks[1].mute && r.master_gain_db == -6.0f,
            "mixer.mixer_fields_roundtrip");
      CHECK(op::save_project(tmp_file("op_mx_mix2.opj"), r, err), "mixer.resave_ok");
      CHECK(read_all(tmp_file("op_mx_mix.opj")) == read_all(tmp_file("op_mx_mix2.opj")),
            "mixer.canonical_resave_stable"); }
}

// --- JSON model fuzz (WP-0122): .opj files arrive from strangers via .opd — no input may crash.
static void test_json_fuzz() {
    std::string err;
    // Rich seed corpus: every field family exercised, built deterministically via save_project.
    op::Project rich;
    rich.bpm = 133.7; rich.length_bars = 6.0; rich.sample_rate = 48000; rich.seed = 7;
    op::Track t0; t0.name = "Metro"; t0.voice.kind = op::VoiceKind::Metronome;
    op::Track t1; t1.name = "Sine"; t1.voice.kind = op::VoiceKind::Sine; t1.voice.freq_hz = 233.0f;
    op::Track t2; t2.name = "Syn"; t2.voice.kind = op::VoiceKind::Synth; t2.voice.attack_ms = 4.0;
    op::Track t3; t3.name = "Sam"; t3.voice.kind = op::VoiceKind::Sampler; t3.voice.path = "kits/k.wav";
    rich.tracks = {t0, t1, t2, t3};
    for (int i = 0; i < 4; ++i) {
        op::Pattern pat; pat.track = i;
        pat.notes.push_back({0.0, 1.5, 60 + i, 0.75});
        pat.notes.push_back({2.0, 0.5, -1, 1.0});
        rich.patterns.push_back(pat);
    }
    rich.arrangement = {{0, 0.0, 3, 0.0}, {1, 2.0, 1, -6.0}, {2, 4.0, 2, 0.0}};
    rich.tempo_map = {{0.0, 133.7}, {4.0, 88.0}};
    rich.attachments = {{"stems/drums.wav", "stem", "demucs"}};
    CHECK(op::save_project(tmp_file("op_fz_seed.opj"), rich, err), "fz.seed_saved");
    std::ifstream f(tmp_file("op_fz_seed.opj"), std::ios::binary);
    const std::string seed((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Invariants any successfully-parsed project must satisfy (spec promises).
    auto invariants = [](const op::Project& p) {
        if (!(p.bpm > 0.0) || p.sample_rate <= 0 || p.beats_per_bar < 1) return false;
        if (p.tracks.empty() && !p.patterns.empty()) return false;
        for (const auto& pat : p.patterns) {
            if (pat.track < 0 || static_cast<size_t>(pat.track) >= p.tracks.size()) return false;
            for (const auto& n : pat.notes) {
                if (!(n.t_beats >= 0.0) || !(n.dur_beats > 0.0)) return false;
                if (n.midi_note < -1 || n.midi_note > 127) return false;
                if (!(n.velocity > 0.0) || n.velocity > 1.0) return false;
            }
        }
        for (const auto& c : p.arrangement) {
            if (c.pattern < 0 || static_cast<size_t>(c.pattern) >= p.patterns.size()) return false;
            if (!(c.start_bar >= 0.0) || c.repeats < 1) return false;
            if (!(c.gain_db >= -96.0) || c.gain_db > 24.0) return false;
        }
        double prev = -1.0;
        bool first_tp = true;
        for (const auto& tp : p.tempo_map) {
            if (!(tp.bar > prev)) return false;                 // strictly increasing bars
            if (first_tp && tp.bar != 0.0) return false;        // anchored at bar 0
            first_tp = false;
            prev = tp.bar;
            if (!(tp.bpm > 0.0) || tp.bpm > 1000.0) return false;
        }
        return true;
    };

    // A load attempt must never escape an exception, whatever the bytes claim.
    auto try_load = [&](const std::string& path) -> int {  // 1 ok / 0 clean-fail / -1 escaped
        op::Project p;
        try {
            return op::load_project(path, p, err) ? (invariants(p) ? 1 : -2) : 0;
        } catch (const std::exception&) {
            return -1;
        } catch (...) {
            return -1;
        }
    };

    // Truncation sweep: every prefix must load or fail cleanly.
    int trunc_crash = 0, trunc_ok = 0;
    for (size_t i = 1; i < seed.size(); ++i) {
        std::ofstream w(tmp_file("op_fz_t.opj"), std::ios::binary);
        w.write(seed.data(), static_cast<std::streamsize>(i));
        w.close();
        const int r = try_load(tmp_file("op_fz_t.opj"));
        if (r < 0) ++trunc_crash; else if (r == 1) ++trunc_ok;
    }
    CHECK(trunc_crash == 0, "fz.truncation_never_escapes");

    // Mutation fuzz: seeded byte flips/inserts/deletions + structural corruptions.
    op::rng::Pcg32 prng(20261118ull);
    static const char kFzChars[] = "{}[]\":,0e-9truefalsnul";
    constexpr size_t kFzN = sizeof(kFzChars) - 1;
    int mut_parsed = 0, mut_failed_clean = 0, mut_crash = 0;
    for (int trial = 0; trial < 500; ++trial) {
        std::string m = seed;
        const int n_edits = 1 + static_cast<int>(prng.next() % 4);
        for (int e = 0; e < n_edits && !m.empty(); ++e) {
            const size_t pos = static_cast<size_t>(prng.next() % static_cast<uint64_t>(m.size()));
            switch (prng.next() % 3) {
                case 0: m[pos] = static_cast<char>(prng.next() % 256u); break;         // flip
                case 1: m.insert(m.begin() + static_cast<long>(pos),                      // insert
                                 kFzChars[prng.next() % kFzN]); break;
                default: m.erase(m.begin() + static_cast<long>(pos)); break;             // delete
            }
        }
        std::ofstream w(tmp_file("op_fz_m.opj"), std::ios::binary);
        w.write(m.data(), static_cast<std::streamsize>(m.size()));
        w.close();
        const int r = try_load(tmp_file("op_fz_m.opj"));
        if (r < 0) ++mut_crash; else if (r == 1) ++mut_parsed; else ++mut_failed_clean;
    }
    CHECK(mut_crash == 0, "fz.mutation_never_escapes");
    CHECK(mut_parsed > 50, "fz.mutation_still_parses_many");  // not a reject-everything stub

    // Type-confusion class (WP-0122): random byte mutations never produce exact key/type swaps,
    // but hand-directed ones found 22 escaping type_error.302 paths before the fix. Every top-level
    // field × wrong-typed value must fail cleanly — this is what strangers can send via .opd.
    {
        int escaped = 0, cases = 0;
        const char* keys[] = {"bpm", "tracks", "patterns", "sample_rate", "length_bars",
                              "arrangement", "tempo_map", "attachments", "seed", "beats_per_bar"};
        const char* bad_vals[] = {"[]", "{}", "\"str\"", "null", "true"};
        for (const char* key : keys) {
            for (const char* v : bad_vals) {
                const size_t kp = seed.find(std::string("\"") + key + "\"");
                if (kp == std::string::npos) continue;
                const size_t colon = seed.find(':', kp);
                const size_t end = seed.find_first_of(",\n", colon);
                std::string m = seed.substr(0, colon + 1) + " " + v + seed.substr(end);
                std::ofstream w(tmp_file("op_fz_tc.opj"), std::ios::binary);
                w << m;
                w.close();
                ++cases;
                if (try_load(tmp_file("op_fz_tc.opj")) < 0) ++escaped;  // -1 escaped, -2 invariant broken
            }
        }
        CHECK(cases >= 40, "fz.type_confusion_coverage");
        CHECK(escaped == 0, "fz.type_confusion_never_escapes");
    }
    std::printf("json fuzz: trunc ok=%d | mut parsed=%d clean-fail=%d crash=0\n",
                trunc_ok, mut_parsed, mut_failed_clean);
}

// --- WAV reader fuzz (WP-0125): sampler loads WAVs from hostile .opd bundles — no crash, no OOB.
static void test_wav_fuzz() {
    // Build a canonical PCM16 stereo seed WAV via the writer, plus float32 twins with crafted
    // fmt fields (the deliberate classes: bits=0 → div-by-zero, bits=24 → OOB read, sr=0).
    op::wav::StereoBuffer buf;
    buf.interleaved.assign(200, 0.25f);
    std::string err;
    CHECK(op::wav::write_pcm16_stereo(tmp_file("op_wf_seed.wav"), buf, err), "wf.seed_saved");

    auto patch_u16 = [](std::vector<uint8_t>& v, size_t off, uint16_t x) {
        v[off] = static_cast<uint8_t>(x & 0xFF);
        v[off + 1] = static_cast<uint8_t>(x >> 8);
    };
    auto patch_u32 = [](std::vector<uint8_t>& v, size_t off, uint32_t x) {
        for (int i = 0; i < 4; ++i) v[off + i] = static_cast<uint8_t>((x >> (8 * i)) & 0xFFu);
    };
    std::ifstream f(tmp_file("op_wf_seed.wav"), std::ios::binary);
    const std::vector<uint8_t> seed((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto load_must_survive = [&](const std::vector<uint8_t>& bytes, const char* label, bool must_reject) {
        std::ofstream w(tmp_file("op_wf_case.wav"), std::ios::binary);
        w.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        w.close();
        op::wav::ReadResult r;
        try {
            r = op::wav::read_wav(tmp_file("op_wf_case.wav"));  // must return, never abort/OOB
        } catch (...) { r.ok = false; }
        if (must_reject) { CHECK(!r.ok, label); return; }  // crafted-invalid: clean rejection required
        if (r.ok) {
            for (float s : r.samples)
                if (!std::isfinite(s) || std::fabs(s) > 16.0f) { CHECK(false, label); return; }
        }
    };

    // Crafted fmt mutations (offsets into canonical header: fmt chunk at 12; tag@20, ch@22, sr@24, bits@34).
    struct Bad { size_t off; uint32_t val; bool wide; const char* label; };
    const Bad bads[] = {
        {20, 3, false, "float_tag"},          // tag=3 with bits=16 → must reject (not read as f32)
        {20, 3, false, "tag3_bits0"},         // combined below with bits patch
        {34, 0, false, "bits0"},              // bps=0 → division by zero pre-fix
        {34, 24, false, "bits24"},            // float tag w/ 24-bit → OOB read pre-fix
        {34, 8, false, "bits8"},
        {24, 0, true, "sr0"},                 // sample rate 0
        {22, 0, false, "ch0"},                // channels 0
        {22, 7, false, "ch7"},                // unsupported channel count
    };
    for (const auto& b : bads) {
        std::vector<uint8_t> m = seed;
        if (b.wide) patch_u32(m, b.off, b.val); else patch_u16(m, b.off, static_cast<uint16_t>(b.val));
        // For float-tag cases also set tag=3 where the label asks for it.
        if (std::string(b.label).find("tag3") != std::string::npos ||
            std::string(b.label) == "bits24" || std::string(b.label) == "bits0" || std::string(b.label) == "bits8")
            patch_u16(m, 20, 3);
        load_must_survive(m, b.label, true);
    }

    // Random structural fuzz over the seed: truncations + byte flips, seeded.
    op::rng::Pcg32 prng(20261119ull);
    int survived = 0;
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<uint8_t> m = seed;
        const size_t cut = static_cast<size_t>(prng.next() % m.size());
        if (trial % 3 == 0) m.resize(cut);  // truncate
        else {
            const int n_edits = 1 + static_cast<int>(prng.next() % 5);
            for (int e = 0; e < n_edits && !m.empty(); ++e) {
                const size_t pos = static_cast<size_t>(prng.next() % m.size());
                m[pos] = static_cast<char>(prng.next() % 256u);
            }
        }
        load_must_survive(m, "wf.random", false);
        ++survived;
    }
    CHECK(survived == 400, "wf.random_all_returned");
}

// --- .opd bundle fuzz (WP-0128): hostile central directories must fail cleanly, never OOB.
static void test_bundle_fuzz() {
    std::string err;
    // Seed: real project + sample + attachment packed into a genuine .opd.
    const std::string dir = tmp_file("op_bf_seed");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/kits");
    write_text(dir + "/song.opj", R"({"format_version":0,"bpm":120,"length_bars":2,
        "tracks":[{"voice":{"kind":"sine","freq_hz":220}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1.5,"note":64}]}],
        "attachments":[{"path":"stems/a.wav","kind":"stem","producer":"x"}]})");
    {   // 128-sample deterministic stereo WAVs: kit sample + stem asset
        op::wav::StereoBuffer b;
        for (int i = 0; i < 64; ++i) b.interleaved.push_back(0.1f * i);
        CHECK(op::wav::write_pcm16_stereo(dir + "/kits/k.wav", b, err), "bf.seed_wav");
    }
    std::filesystem::create_directories(dir + "/stems");
    { op::wav::StereoBuffer b; for (int i = 0; i < 64; ++i) b.interleaved.push_back(0.2f * i);
      CHECK(op::wav::write_pcm16_stereo(dir + "/stems/a.wav", b, err), "bf.stem"); }
    CHECK(op::pack_project(dir + "/song.opj", dir + "/seed.opd", err), "bf.seed_packed");
    std::ifstream f(dir + "/seed.opd", std::ios::binary);
    const std::vector<uint8_t> seed((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(seed.size() > 200, "bf.seed_size");

    auto unpack_must_survive = [&](const std::vector<uint8_t>& bytes, const char* label) {
        const std::string p = dir + "/case.opd";
        std::ofstream w(p, std::ios::binary);
        w.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        w.close();
        std::string main_opj;
        std::vector<std::string> entries;
        try {
            op::list_bundle(p, entries, err);   // both readers get every case
            op::unpack_bundle(p, dir + "/out", main_opj, err);
        } catch (...) { CHECK(false, label); }
    };

    auto patch16 = [](std::vector<uint8_t>& v, size_t off, uint16_t x) {
        if (off + 2 <= v.size()) { v[off] = static_cast<uint8_t>(x & 0xFF); v[off + 1] = static_cast<uint8_t>(x >> 8); }
    };
    auto patch32 = [](std::vector<uint8_t>& v, size_t off, uint32_t x) {
        for (int i = 0; i < 4 && off + i < v.size(); ++i) v[off + i] = static_cast<uint8_t>((x >> (8 * i)) & 0xFFu);
    };

    // Crafted: nlen=0xFFFF on first central entry near EOF (OOB candidate), bogus cd_off,
    // inflated n_entries, data-corruption (must be a clean CRC reject, not a crash).
    {
        const long eocd = static_cast<long>(seed.size()) - 22;
        CHECK(eocd > 0 && seed[eocd] == 'P' && seed[eocd + 1] == 'K', "bf.seed_eocd_tail");
        const uint32_t cd_off = static_cast<uint32_t>(seed[eocd + 16]) | (seed[eocd + 17] << 8) |
                                (static_cast<uint32_t>(seed[eocd + 18]) << 16) |
                                (static_cast<uint32_t>(seed[eocd + 19]) << 24);
        std::vector<uint8_t> m = seed;
        patch16(m, cd_off + 28, 0xFFFF);          // huge name length on first entry → OOB read pre-fix
        unpack_must_survive(m, "bf.crafted_nlen");
        m = seed; patch32(m, static_cast<size_t>(eocd) + 16, 0xFFFFFFF0u);  // cd_off past EOF
        unpack_must_survive(m, "bf.crafted_cdoff");
        m = seed; patch16(m, static_cast<size_t>(eocd) + 10, 300);          // more entries than exist
        unpack_must_survive(m, "bf.crafted_ncount");
        m = seed; if (m.size() > 100) m[50] ^= 0xFF;                        // corrupt entry data
        unpack_must_survive(m, "bf.crafted_corrupt_data");
    }

    // Truncation sweep: every byte of the tail region + coarse steps through the body.
    for (size_t i = 1; i < seed.size(); i += (i > seed.size() - 512) ? 1 : 37) {
        unpack_must_survive(std::vector<uint8_t>(seed.begin(), seed.begin() + static_cast<long>(i)), "bf.trunc");
    }

    // Seeded byte mutations biased to the central directory / EOCD tail.
    op::rng::Pcg32 prng(20261121ull);
    for (int trial = 0; trial < 400; ++trial) {
        std::vector<uint8_t> m = seed;
        const int n_edits = 1 + static_cast<int>(prng.next() % 4);
        for (int e = 0; e < n_edits; ++e) {
            size_t pos = (prng.next() % 2 == 0 && m.size() > 2048)
                             ? m.size() - 1 - static_cast<size_t>(prng.next() % 2048)  // tail-biased
                             : static_cast<size_t>(prng.next() % m.size());
            m[pos] = static_cast<char>(prng.next() % 256u);
        }
        unpack_must_survive(m, "bf.mutate");
    }
    std::filesystem::remove_all(dir + "/out");
}

// --- MIDI export flattens arrangement (WP-0129).
static void test_arrangement_export() {
    std::string err;
    op::Project p;
    p.bpm = 120.0; p.length_bars = 8.0;
    op::Track t; t.name = "L"; t.voice.kind = op::VoiceKind::Synth;
    p.tracks.push_back(t);
    op::Pattern pat; pat.track = 0;
    pat.notes.push_back({0.0, 1.0, 60, 1.0});
    pat.notes.push_back({1.0, 1.0, 64, 1.0});   // extent = 2 beats
    p.patterns.push_back(pat);

    // No arrangement: legacy placement (every pattern once at bar 0).
    CHECK(op::save_midi(tmp_file("op_ae_legacy.mid"), p, err), "ae.legacy_saved");
    op::Project back_l;
    CHECK(op::load_midi(tmp_file("op_ae_legacy.mid"), back_l, err), "ae.legacy_reloaded");
    {
        size_t notes = 0;
        for (const auto& q : back_l.patterns) notes += q.notes.size();
        CHECK(notes == 2, "ae.legacy_two_notes");
    }

    // Arrangement: clip@bar0 + clip@bar2×3 with -6.02 dB on the second → 8 note events total;
    // reps spaced by extent (2 beats) exactly like the renderer; gain baked into velocity (~64).
    p.arrangement = {{0, 0.0, 1, 0.0}, {0, 2.0, 3, -6.0206}};
    CHECK(op::save_midi(tmp_file("op_ae_flat.mid"), p, err), "ae.flat_saved");
    op::Project back;
    CHECK(op::load_midi(tmp_file("op_ae_flat.mid"), back, err), "ae.flat_reloaded");
    std::vector<double> onsets;
    int quiet = 0, loud = 0;
    for (const auto& q : back.patterns) {
        for (const auto& n : q.notes) {
            onsets.push_back(n.t_beats);
            if (n.velocity < 0.6) ++quiet; else ++loud;
        }
    }
    CHECK(onsets.size() == 8, "ae.eight_notes");
    std::sort(onsets.begin(), onsets.end());
    // Sorted onsets: [0,1, 8,9, 10,11, 12,13] — clip@bar2=beat8, reps spaced by extent (2 beats).
    const bool spacing_ok = onsets.size() == 8 && std::fabs(onsets[2] - 8.0) < 1e-6 &&
                            std::fabs(onsets[4] - 10.0) < 1e-6 && std::fabs(onsets[6] - 12.0) < 1e-6;
    CHECK(spacing_ok, "ae.rep_offsets_match_renderer");
    CHECK(loud == 2 && quiet == 6, "ae.gain_baked_into_velocity");

    // Determinism: same project → byte-identical MIDI.
    CHECK(op::save_midi(tmp_file("op_ae_flat2.mid"), p, err), "ae.save2_ok");
    {
        auto rb = [](const std::string& path) {
            std::ifstream f(path, std::ios::binary);
            return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        };
        CHECK(rb(tmp_file("op_ae_flat.mid")) == rb(tmp_file("op_ae_flat2.mid")), "ae.export_deterministic");
    }
}

// --- model operations + undo/redo (WP-0135): docs/spec/operations.md conformance.
static std::vector<uint8_t> slurp_bytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// apply_op must reject o AND leave the project's saved bytes identical across the attempt.
static bool rejected_unchanged(op::Project& p, const nlohmann::json& o, std::string& err) {
    std::string serr;
    if (!op::save_project(tmp_file("op_rej_before.opj"), p, serr)) return false;
    if (op::apply_op(p, o, err)) return false;  // must be rejected
    if (!op::save_project(tmp_file("op_rej_after.opj"), p, serr)) return false;
    return slurp_bytes(tmp_file("op_rej_before.opj")) == slurp_bytes(tmp_file("op_rej_after.opj"));
}

// The exact bytes save_project writes for p (WP-0147 byte-exact undo/redo and rejection checks).
static std::string saved_text(const op::Project& p) {
    std::string serr;
    if (!op::save_project(tmp_file("op_saved_text.opj"), p, serr)) return "";
    const std::vector<uint8_t> b = slurp_bytes(tmp_file("op_saved_text.opj"));
    return std::string(b.begin(), b.end());
}

static void test_ops() {
    std::string err;
    op::Project p;
    p.bpm = 120.0; p.length_bars = 4.0; p.sample_rate = 48000; p.seed = 5;
    op::Track t0; t0.name = "Metro"; t0.voice.kind = op::VoiceKind::Metronome;
    op::Track t1; t1.name = "Sine"; t1.voice.kind = op::VoiceKind::Sine; t1.voice.freq_hz = 220.0f;
    p.tracks = {t0, t1};
    op::Pattern pm; pm.track = 0; pm.notes.push_back({0.0, 0.25});
    op::Pattern ps; ps.track = 1; ps.notes.push_back({0.0, 1.0});
    p.patterns = {pm, ps};
    p.arrangement = {{0, 0.0, 2, 0.0}};  // clip references pattern 0

    using nlohmann::json;
    // set_bpm / set_length_bars
    CHECK(op::apply_op(p, json{{"op", "set_bpm"}, {"bpm", 95.0}}, err) && p.bpm == 95.0, "ops.set_bpm");
    CHECK(rejected_unchanged(p, json{{"op", "set_bpm"}, {"bpm", -3.0}}, err) &&
              err == "set_bpm: bpm must be > 0", "ops.set_bpm_reject_err_format");
    CHECK(op::apply_op(p, json{{"op", "set_length_bars"}, {"length_bars", 8.0}}, err) &&
              p.length_bars == 8.0, "ops.set_length_bars");
    CHECK(rejected_unchanged(p, json{{"op", "set_length_bars"}, {"length_bars", 0.0}}, err),
          "ops.set_length_bars_reject");

    // add_track / remove_track (in-use + out-of-range)
    CHECK(op::apply_op(p, json{{"op", "add_track"},
                               {"track", {{"name", "Extra"}, {"voice", {{"kind", "sine"}, {"freq_hz", 110.0}}}}}},
                       err) && p.tracks.size() == 3, "ops.add_track");
    CHECK(rejected_unchanged(p, json{{"op", "add_track"},
                                     {"track", {{"name", "X"}, {"voice", {{"kind", "koto"}}}}}}, err),
          "ops.add_track_reject_bad_voice");
    CHECK(rejected_unchanged(p, json{{"op", "remove_track"}, {"index", 0}}, err) &&
              err == "remove_track: track in use", "ops.remove_track_in_use");
    CHECK(op::apply_op(p, json{{"op", "remove_track"}, {"index", 2}}, err) && p.tracks.size() == 2,
          "ops.remove_track_unreferenced");
    CHECK(rejected_unchanged(p, json{{"op", "remove_track"}, {"index", 99}}, err),
          "ops.remove_track_reject_oob");

    // set_track_gain / set_track_pan / rename_track
    CHECK(op::apply_op(p, json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -12.0}}, err) &&
              p.tracks[0].gain_db == -12.0f, "ops.set_track_gain");
    CHECK(rejected_unchanged(p, json{{"op", "set_track_gain"}, {"index", 99}, {"gain_db", 3.0}}, err),
          "ops.set_track_gain_reject_oob");
    CHECK(op::apply_op(p, json{{"op", "set_track_pan"}, {"index", 1}, {"pan", -0.5}}, err) &&
              p.tracks[1].pan == -0.5f, "ops.set_track_pan");
    CHECK(rejected_unchanged(p, json{{"op", "set_track_pan"}, {"index", 1}, {"pan", "left"}}, err),
          "ops.set_track_pan_reject_type");
    CHECK(op::apply_op(p, json{{"op", "rename_track"}, {"index", 0}, {"name", "Click"}}, err) &&
              p.tracks[0].name == "Click", "ops.rename_track");
    CHECK(rejected_unchanged(p, json{{"op", "rename_track"}, {"index", 0}, {"name", 42}}, err),
          "ops.rename_track_reject_type");

    // Mixer ops (WP-0136): mute/solo/master + parser range validation inherited by ops.
    CHECK(op::apply_op(p, json{{"op", "set_track_mute"}, {"index", 1}, {"mute", true}}, err) &&
              p.tracks[1].mute, "ops.set_track_mute");
    CHECK(rejected_unchanged(p, json{{"op", "set_track_mute"}, {"index", 1}, {"mute", "yes"}}, err) &&
              err == "set_track_mute: 'mute' must be a boolean", "ops.set_track_mute_reject_type");
    CHECK(op::apply_op(p, json{{"op", "set_track_solo"}, {"index", 0}, {"solo", true}}, err) &&
              p.tracks[0].solo, "ops.set_track_solo");
    CHECK(rejected_unchanged(p, json{{"op", "set_track_solo"}, {"index", 99}, {"solo", true}}, err),
          "ops.set_track_solo_reject_oob");
    CHECK(op::apply_op(p, json{{"op", "set_master_gain"}, {"gain_db", -6.0}}, err) &&
              p.master_gain_db == -6.0f, "ops.set_master_gain");
    CHECK(rejected_unchanged(p, json{{"op", "set_master_gain"}, {"gain_db", 30.0}}, err) &&
              err == "set_master_gain: master_gain_db out of range [-96, 24]",
          "ops.set_master_gain_reject_range");
    // Gap closure (WP-0135 review): the parser now bounds track gain/pan; ops inherit it verbatim.
    CHECK(rejected_unchanged(p, json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", 99.0}}, err) &&
              err == "set_track_gain: track.gain_db out of range [-96, 24]",
          "ops.set_track_gain_reject_range");
    CHECK(rejected_unchanged(p, json{{"op", "set_track_pan"}, {"index", 1}, {"pan", 5.0}}, err) &&
              err == "set_track_pan: track.pan out of range [-1, 1]", "ops.set_track_pan_reject_range");

    // add_pattern / remove_pattern (in-use via clips)
    CHECK(op::apply_op(p, json{{"op", "add_pattern"},
                                {"pattern", {{"track", 1}, {"notes", json::array({{{"t", 0.0}, {"dur", 0.5}}})}}}},
                       err) && p.patterns.size() == 3, "ops.add_pattern");
    CHECK(rejected_unchanged(p, json{{"op", "add_pattern"},
                                     {"pattern", {{"track", 99}, {"notes", json::array()}}}}, err),
          "ops.add_pattern_reject_track_oob");
    CHECK(rejected_unchanged(p, json{{"op", "remove_pattern"}, {"index", 0}}, err) &&
              err == "remove_pattern: pattern in use", "ops.remove_pattern_in_use");
    CHECK(op::apply_op(p, json{{"op", "remove_pattern"}, {"index", 2}}, err) && p.patterns.size() == 2,
          "ops.remove_pattern_unreferenced");

    // add_note / remove_note
    CHECK(op::apply_op(p, json{{"op", "add_note"}, {"pattern", 1},
                                {"note", {{"t", 1.5}, {"dur", 0.5}}}}, err) &&
              p.patterns[1].notes.size() == 2, "ops.add_note");
    CHECK(rejected_unchanged(p, json{{"op", "add_note"}, {"pattern", 1},
                                     {"note", {{"t", 0.0}, {"dur", -1.0}}}}, err),
          "ops.add_note_reject_bad_dur");
    CHECK(rejected_unchanged(p, json{{"op", "remove_note"}, {"pattern", 1}, {"index", 99}}, err),
          "ops.remove_note_reject_oob");
    CHECK(op::apply_op(p, json{{"op", "remove_note"}, {"pattern", 1}, {"index", 1}}, err) &&
              p.patterns[1].notes.size() == 1, "ops.remove_note");

    // add_clip / move_clip / set_clip_gain / remove_clip
    CHECK(op::apply_op(p, json{{"op", "add_clip"},
                                {"clip", {{"pattern", 1}, {"start_bar", 2.0}, {"repeats", 1}}}}, err) &&
              p.arrangement.size() == 2, "ops.add_clip");
    CHECK(rejected_unchanged(p, json{{"op", "add_clip"},
                                     {"clip", {{"pattern", 0}, {"start_bar", 0.0}, {"repeats", 0}}}}, err),
          "ops.add_clip_reject_bad_repeats");
    CHECK(op::apply_op(p, json{{"op", "move_clip"}, {"index", 1}, {"start_bar", 3.5}}, err) &&
              p.arrangement[1].start_bar == 3.5, "ops.move_clip");
    CHECK(rejected_unchanged(p, json{{"op", "move_clip"}, {"index", 1}, {"start_bar", -1.0}}, err),
          "ops.move_clip_reject_negative");
    CHECK(op::apply_op(p, json{{"op", "set_clip_gain"}, {"index", 1}, {"gain_db", -6.0}}, err) &&
              p.arrangement[1].gain_db == -6.0, "ops.set_clip_gain");
    CHECK(rejected_unchanged(p, json{{"op", "set_clip_gain"}, {"index", 1}, {"gain_db", 30.0}}, err),
          "ops.set_clip_gain_reject_out_of_range");
    CHECK(rejected_unchanged(p, json{{"op", "remove_clip"}, {"index", 99}}, err),
          "ops.remove_clip_reject_oob");
    CHECK(op::apply_op(p, json{{"op", "remove_clip"}, {"index", 1}}, err) && p.arrangement.size() == 1,
          "ops.remove_clip");

    // WP-0147 clip ops: set_clip_repeats / set_clip_pattern / duplicate_clip / reorder_clip.
    // Each check starts from a fresh 3-clip arrangement (the project above has been mutated).
    {
        auto fresh = [&]() {
            op::Project f;
            f.bpm = 120.0; f.length_bars = 8.0; f.sample_rate = 48000; f.seed = 5;
            f.tracks = {t0, t1};
            f.patterns = {pm, ps};
            f.arrangement = {{0, 0.0, 2, -3.0}, {1, 1.0, 1, 0.0}, {0, 2.0, 3, -6.0}};
            return f;
        };
        auto same_clip = [](const op::Clip& a, const op::Clip& b) {
            return a.pattern == b.pattern && a.start_bar == b.start_bar && a.repeats == b.repeats &&
                   a.gain_db == b.gain_db;
        };

        // set_clip_repeats
        {
            op::Project f = fresh();
            const auto before = f.arrangement;
            CHECK(op::apply_op(f, json{{"op", "set_clip_repeats"}, {"index", 1}, {"repeats", 4}}, err) &&
                      f.arrangement[1].repeats == 4 && f.arrangement[1].pattern == 1 &&
                      f.arrangement[1].start_bar == 1.0 && f.arrangement[1].gain_db == 0.0 &&
                      same_clip(f.arrangement[0], before[0]) && same_clip(f.arrangement[2], before[2]),
                  "ops.set_clip_repeats");
            op::Project g = fresh();
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_repeats"}, {"index", 1}, {"repeats", 0}}, err) &&
                      err == "set_clip_repeats: clip.repeats out of range [1..4096]",
                  "ops.set_clip_repeats_reject_zero");
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_repeats"}, {"index", 1}, {"repeats", 4097}}, err) &&
                      err == "set_clip_repeats: clip.repeats out of range [1..4096]",
                  "ops.set_clip_repeats_reject_4097");
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_repeats"}, {"index", 1}, {"repeats", 4294967297LL}}, err) &&
                      err == "set_clip_repeats: clip.repeats out of range [1..4096]",
                  "ops.set_clip_repeats_reject_no_wrap");
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_repeats"}, {"index", 1}, {"repeats", 3.0}}, err) &&
                      err == "set_clip_repeats: 'repeats' must be an integer",
                  "ops.set_clip_repeats_reject_float");
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_repeats"}, {"index", 1}}, err) &&
                      err == "set_clip_repeats: missing argument 'repeats'",
                  "ops.set_clip_repeats_reject_missing");
        }

        // set_clip_pattern
        {
            op::Project f = fresh();
            const auto before = f.arrangement;
            CHECK(op::apply_op(f, json{{"op", "set_clip_pattern"}, {"index", 0}, {"pattern", 1}}, err) &&
                      f.arrangement[0].pattern == 1 && f.arrangement[0].start_bar == 0.0 &&
                      f.arrangement[0].repeats == 2 && f.arrangement[0].gain_db == -3.0 &&
                      same_clip(f.arrangement[1], before[1]) && same_clip(f.arrangement[2], before[2]),
                  "ops.set_clip_pattern");
            op::Project g = fresh();
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_pattern"}, {"index", 0}, {"pattern", -1}}, err) &&
                      err == "set_clip_pattern: clip.pattern out of range: -1",
                  "ops.set_clip_pattern_reject_negative");
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_pattern"}, {"index", 0}, {"pattern", 2}}, err) &&
                      err == "set_clip_pattern: clip.pattern out of range: 2",
                  "ops.set_clip_pattern_reject_oob_pattern");
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_pattern"}, {"index", 0}, {"pattern", 4294967297LL}}, err) &&
                      err == "set_clip_pattern: clip.pattern out of range: 4294967297",
                  "ops.set_clip_pattern_reject_no_wrap");
        }

        // duplicate_clip: copy inserted at index+1, start_bar replaced, other fields copied.
        {
            op::Project f = fresh();
            const auto before = f.arrangement;
            CHECK(op::apply_op(f, json{{"op", "duplicate_clip"}, {"index", 0}, {"start_bar", 4.0}}, err) &&
                      f.arrangement.size() == 4 && f.arrangement[1].start_bar == 4.0 &&
                      f.arrangement[1].pattern == before[0].pattern &&
                      f.arrangement[1].repeats == before[0].repeats,
                  "ops.duplicate_clip");
            CHECK(f.arrangement[1].gain_db == -3.0, "ops.duplicate_clip_copies_gain");
            CHECK(same_clip(f.arrangement[0], before[0]) && same_clip(f.arrangement[2], before[1]) &&
                      same_clip(f.arrangement[3], before[2]),
                  "ops.duplicate_clip_keeps_others");
            op::Project g = fresh();
            CHECK(rejected_unchanged(g, json{{"op", "duplicate_clip"}, {"index", 0}, {"start_bar", "4"}}, err) &&
                      err == "duplicate_clip: 'start_bar' must be a number",
                  "ops.duplicate_clip_reject_type");
            CHECK(rejected_unchanged(g, json{{"op", "duplicate_clip"}, {"index", 0}, {"start_bar", -1.0}}, err),
                  "ops.duplicate_clip_reject_negative");
        }

        // reorder_clip: final position `to`; others keep relative order.
        {
            op::Project f = fresh();
            const auto before = f.arrangement;
            CHECK(op::apply_op(f, json{{"op", "reorder_clip"}, {"index", 0}, {"to", 2}}, err) &&
                      f.arrangement.size() == 3 && same_clip(f.arrangement[0], before[1]) &&
                      same_clip(f.arrangement[1], before[2]) && same_clip(f.arrangement[2], before[0]),
                  "ops.reorder_clip_to_last");
            op::Project f2 = fresh();
            CHECK(op::apply_op(f2, json{{"op", "reorder_clip"}, {"index", 2}, {"to", 0}}, err) &&
                      same_clip(f2.arrangement[0], before[2]) && same_clip(f2.arrangement[1], before[0]) &&
                      same_clip(f2.arrangement[2], before[1]),
                  "ops.reorder_clip_to_first");
            op::Project f3 = fresh();
            const std::string t_before = saved_text(f3);
            CHECK(op::apply_op(f3, json{{"op", "reorder_clip"}, {"index", 1}, {"to", 1}}, err) &&
                      saved_text(f3) == t_before, "ops.reorder_clip_to_self");
            op::Project g = fresh();
            CHECK(rejected_unchanged(g, json{{"op", "reorder_clip"}, {"index", 0}, {"to", 3}}, err) &&
                      err == "reorder_clip: 'to' out of range",
                  "ops.reorder_clip_reject_to_oob");
        }

        // Index strictness for all four ops: 3 and -1 are out of range for a 3-clip arrangement.
        {
            op::Project g = fresh();
            CHECK(rejected_unchanged(g, json{{"op", "set_clip_repeats"}, {"index", 3}, {"repeats", 2}}, err) &&
                      rejected_unchanged(g, json{{"op", "set_clip_repeats"}, {"index", -1}, {"repeats", 2}}, err) &&
                      rejected_unchanged(g, json{{"op", "set_clip_pattern"}, {"index", 3}, {"pattern", 0}}, err) &&
                      rejected_unchanged(g, json{{"op", "set_clip_pattern"}, {"index", -1}, {"pattern", 0}}, err) &&
                      rejected_unchanged(g, json{{"op", "duplicate_clip"}, {"index", 3}, {"start_bar", 1.0}}, err) &&
                      rejected_unchanged(g, json{{"op", "duplicate_clip"}, {"index", -1}, {"start_bar", 1.0}}, err) &&
                      rejected_unchanged(g, json{{"op", "reorder_clip"}, {"index", 3}, {"to", 0}}, err) &&
                      rejected_unchanged(g, json{{"op", "reorder_clip"}, {"index", -1}, {"to", 0}}, err),
                  "ops.clip_ops_reject_bad_index");
        }

        // History: each op undoes and redoes byte-exactly.
        auto roundtrip = [&](const json& o) {
            op::Project f = fresh();
            op::History hh;
            const std::string before_txt = saved_text(f);
            if (!hh.apply(f, o, err)) return false;
            const std::string after_txt = saved_text(f);
            if (after_txt == before_txt) return false;
            return hh.undo(f) && saved_text(f) == before_txt && hh.redo(f) && saved_text(f) == after_txt;
        };
        CHECK(roundtrip(json{{"op", "set_clip_repeats"}, {"index", 1}, {"repeats", 4}}), "hist.set_clip_repeats");
        CHECK(roundtrip(json{{"op", "set_clip_pattern"}, {"index", 0}, {"pattern", 1}}), "hist.set_clip_pattern");
        CHECK(roundtrip(json{{"op", "duplicate_clip"}, {"index", 0}, {"start_bar", 4.0}}), "hist.duplicate_clip");
        CHECK(roundtrip(json{{"op", "reorder_clip"}, {"index", 0}, {"to", 2}}), "hist.reorder_clip");

        // Journal: mixed new ops are all-or-nothing.
        {
            op::Project f = fresh();
            const std::string before_txt = saved_text(f);
            const std::string bad_journal =
                R"J({"op":"duplicate_clip","index":0,"start_bar":4.0})J" "\n"
                R"J({"op":"reorder_clip","index":0,"to":2})J" "\n"
                R"J({"op":"set_clip_repeats","index":1,"repeats":0})J" "\n";
            op::JournalOutcome oc{};
            CHECK(!op::apply_journal(f, bad_journal, err, &oc) &&
                      err.rfind("line 3: set_clip_repeats: clip.repeats out of range", 0) == 0 &&
                      oc.failure == op::JournalFailure::kRejected && saved_text(f) == before_txt,
                  "ops.journal_clip_ops_all_or_nothing");
            const std::string good_journal =
                R"J({"op":"duplicate_clip","index":0,"start_bar":4.0})J" "\n"
                R"J({"op":"reorder_clip","index":0,"to":2})J" "\n";
            op::JournalOutcome ok{};
            CHECK(op::apply_journal(f, good_journal, err, &ok) && ok.applied == 2 && f.arrangement.size() == 4 &&
                      f.arrangement[0].start_bar == 4.0 && f.arrangement[1].start_bar == 1.0 &&
                      f.arrangement[2].start_bar == 0.0,
                  "ops.journal_clip_ops_applied");
        }
    }

    // add_attachment: same rules as op::add_attachment (valid + no duplicate path)
    CHECK(op::apply_op(p, json{{"op", "add_attachment"}, {"path", "stems/x.wav"},
                                {"kind", "stem"}, {"producer", "wp0135"}}, err) &&
              p.attachments.size() == 1, "ops.add_attachment");
    CHECK(rejected_unchanged(p, json{{"op", "add_attachment"}, {"path", "stems/x.wav"},
                                     {"kind", "stem"}}, err),
          "ops.add_attachment_reject_duplicate");
    CHECK(rejected_unchanged(p, json{{"op", "add_attachment"}, {"path", "y.wav"},
                                     {"kind", "magic"}}, err),
          "ops.add_attachment_reject_bad_kind");

    // index strictness (spec landmine): floats rejected even when integral; negatives rejected.
    CHECK(!op::apply_op(p, json{{"op", "set_track_gain"}, {"index", 1.5}, {"gain_db", 0.0}}, err),
          "ops.reject_float_index");
    CHECK(!op::apply_op(p, json{{"op", "set_track_gain"}, {"index", 2.0}, {"gain_db", 0.0}}, err),
          "ops.reject_integral_float_index");
    CHECK(!op::apply_op(p, json{{"op", "remove_clip"}, {"index", -1}}, err), "ops.reject_negative_index");

    // unknown op / unknown extra key
    CHECK(rejected_unchanged(p, json{{"op", "frobnicate"}}, err) && err.find("unknown op") != std::string::npos,
          "ops.reject_unknown_op");
    CHECK(rejected_unchanged(p, json{{"op", "set_bpm"}, {"bpm", 90.0}, {"extra", 1}}, err),
          "ops.reject_unknown_extra_key");

    // --- History: apply/undo/redo/clear-redo-on-new-apply/cap-256 drops oldest.
    {
        op::Project h = p;
        op::History hist;
        CHECK(!hist.can_undo() && !hist.undo(h) && !hist.can_redo() && !hist.redo(h),
              "hist.empty_noop_unchanged");
        const double bpm0 = h.bpm;
        CHECK(hist.apply(h, json{{"op", "set_bpm"}, {"bpm", 90.0}}, err) && h.bpm == 90.0 &&
                  hist.can_undo() && hist.undo_depth() == 1, "hist.apply");
        CHECK(hist.apply(h, json{{"op", "set_bpm"}, {"bpm", 100.0}}, err) && h.bpm == 100.0 &&
                  hist.undo_depth() == 2, "hist.apply2");
        CHECK(hist.undo(h) && h.bpm == 90.0 && hist.can_redo(), "hist.undo");
        CHECK(hist.redo(h) && h.bpm == 100.0 && !hist.can_redo(), "hist.redo");
        CHECK(hist.undo(h), "hist.undo_again");
        const size_t depth_before = hist.undo_depth();
        bool redo_before = hist.can_redo();
        std::string ferr;
        CHECK(!hist.apply(h, json{{"op", "set_bpm"}, {"bpm", -1.0}}, ferr) &&
                  hist.undo_depth() == depth_before && hist.can_redo() == redo_before,
              "hist.failed_apply_touches_nothing");
        CHECK(hist.apply(h, json{{"op", "set_bpm"}, {"bpm", 110.0}}, err) && !hist.can_redo(),
              "hist.new_apply_clears_redo");

        op::Project c = p;
        op::History hc;
        bool cap_ok = true;
        for (int i = 1; i <= 300; ++i)
            if (!hc.apply(c, json{{"op", "set_bpm"}, {"bpm", static_cast<double>(i)}}, err)) cap_ok = false;
        CHECK(cap_ok && hc.undo_depth() == 256, "hist.cap_256");
        while (hc.undo(c)) {}
        CHECK(c.bpm == 44.0, "hist.cap_drops_oldest");  // last 256 of 300 snapshots kept
        while (hc.redo(c)) {}
        CHECK(c.bpm == 300.0, "hist.redo_all_after_cap");
        CHECK(bpm0 > 0.0, "hist.sanity");
    }
}

// Property test: seeded random op sequences against the golden demo fixture. After every
// sequence: undo-all → saved bytes equal the original's; redo-all → equal the forward result's.
static nlohmann::json random_op(op::rng::Pcg32& rng, const op::Project& p) {
    using nlohmann::json;
    const auto pick = [&](size_t n) {
        return n == 0 ? size_t{0} : static_cast<size_t>(rng.next() % static_cast<uint32_t>(n));
    };
    // ~1 in 4 indices are deliberately wild (negative or past the end): clean rejection is part
    // of the contract being tested. Gains occasionally out of range for the same reason.
    const bool wild = (rng.next() & 3u) == 0;
    const auto idx = [&](size_t n) -> long long {
        if (n == 0) return -1;  // empty array: every index is invalid → clean rejection
        return wild ? static_cast<long long>(rng.next() % 64u) - 8 : static_cast<long long>(pick(n));
    };
    const auto gain_db = [&]() { return (rng.next() & 7u) == 0 ? 1e9 : static_cast<double>(pick(48)) - 24.0; };
    const static json voices[] = {
        {{"kind", "metronome"}},
        {{"kind", "sine"}, {"freq_hz", 110.0}},
        {{"kind", "synth"}, {"attack_ms", 3.0}, {"release_ms", 20.0}},
        {{"kind", "sampler"}, {"path", "kits/kick.wav"}},
    };
    switch (pick(19)) {  // +3 mixer ops (WP-0136); default = add_attachment
        case 0: return {{"op", "set_bpm"}, {"bpm", (rng.next() & 7u) == 0 ? 0.0 : 40.0 + pick(200)}};
        case 1: return {{"op", "set_length_bars"},
                        {"length_bars", (rng.next() & 7u) == 0 ? -2.0 : 1.0 + pick(64)}};
        case 2: return {{"op", "add_track"},
                        {"track", {{"name", "T" + std::to_string(pick(1000))},
                                   {"gain_db", gain_db()}, {"pan", static_cast<double>(pick(3)) - 1},
                                   {"voice", voices[pick(4)]}}}};
        case 3: return {{"op", "remove_track"}, {"index", idx(p.tracks.size())}};
        case 4: return {{"op", "set_track_gain"}, {"index", idx(p.tracks.size())}, {"gain_db", gain_db()}};
        case 5: return {{"op", "set_track_pan"}, {"index", idx(p.tracks.size())},
                        {"pan", static_cast<double>(pick(201)) / 100.0 - 1.0}};
        case 6: return {{"op", "rename_track"}, {"index", idx(p.tracks.size())},
                        {"name", "N" + std::to_string(pick(1000))}};
        case 7: return {{"op", "add_pattern"},
                        {"pattern", {{"track", static_cast<long long>(pick(p.tracks.size()))},
                                     {"notes", json::array({{{"t", static_cast<double>(pick(16))},
                                                             {"dur", 0.25}}})}}}};
        case 8: return {{"op", "remove_pattern"}, {"index", idx(p.patterns.size())}};
        case 9: return {{"op", "add_note"}, {"pattern", idx(p.patterns.size())},
                        {"note", {{"t", static_cast<double>(pick(32))},
                                  {"dur", (rng.next() & 7u) == 0 ? 0.0 : 0.25}}}};
        case 10: {
            if (p.patterns.empty()) return json{{"op", "remove_note"}, {"pattern", -1}, {"index", 0}};
            const size_t pi = pick(p.patterns.size());
            return {{"op", "remove_note"}, {"pattern", static_cast<long long>(pi)},
                    {"index", idx(p.patterns[pi].notes.size())}};
        }
        case 11: return {{"op", "add_clip"},
                         {"clip", {{"pattern", static_cast<long long>(pick(p.patterns.size()))},
                                   {"start_bar", static_cast<double>(pick(32))},
                                   {"repeats", 1 + pick(4)}}}};
        case 12: return {{"op", "move_clip"}, {"index", idx(p.arrangement.size())},
                         {"start_bar", wild ? -1.0 : static_cast<double>(pick(64))}};
        case 13: return {{"op", "set_clip_gain"}, {"index", idx(p.arrangement.size())},
                         {"gain_db", gain_db()}};
        case 14: return {{"op", "remove_clip"}, {"index", idx(p.arrangement.size())}};
        case 15: return {{"op", "set_track_mute"}, {"index", idx(p.tracks.size())},
                         {"mute", (rng.next() & 1u) == 0}};
        case 16: return {{"op", "set_track_solo"}, {"index", idx(p.tracks.size())},
                         {"solo", (rng.next() & 1u) == 0}};
        case 17: return {{"op", "set_master_gain"},
                         {"gain_db", (rng.next() & 7u) == 0 ? 99.0 : static_cast<double>(pick(48)) - 24.0}};
        default: return {{"op", "add_attachment"}, {"path", "stems/a" + std::to_string(pick(8)) + ".wav"},
                         {"kind", "stem"}, {"producer", "prop"}};
    }
}

static void test_ops_property() {
    const std::string fix = std::string(OP_TEST_DATA) + "/fixtures/goldens/demo.opj";
    std::string err;
    op::Project orig;
    CHECK(op::load_project(fix, orig, err), "prop.load_fixture");
    CHECK(op::save_project(tmp_file("op_prop_orig.opj"), orig, err), "prop.orig_saved");
    const auto orig_bytes = slurp_bytes(tmp_file("op_prop_orig.opj"));

    op::rng::Pcg32 rng(20261118ull);  // fixed seed: the whole suite is reproducible run-to-run
    long applied_total = 0, rejected_total = 0;
    bool undo_ok = true, redo_ok = true, save_ok = true;
    constexpr int kSeqs = 500;
    for (int trial = 0; trial < kSeqs && undo_ok && redo_ok; ++trial) {
        op::Project p;
        if (!op::load_project(fix, p, err)) { save_ok = false; break; }
        op::History h;
        const int len = 1 + static_cast<int>(rng.next() % 40u);  // sequence length 1..40 (< cap)
        for (int k = 0; k < len; ++k) {
            if (h.apply(p, random_op(rng, p), err)) ++applied_total; else ++rejected_total;
        }
        save_ok = save_ok && op::save_project(tmp_file("op_prop_fwd.opj"), p, err);
        const auto fwd_bytes = slurp_bytes(tmp_file("op_prop_fwd.opj"));
        while (h.undo(p)) {}
        save_ok = save_ok && op::save_project(tmp_file("op_prop_back.opj"), p, err);
        if (slurp_bytes(tmp_file("op_prop_back.opj")) != orig_bytes) undo_ok = false;
        while (h.redo(p)) {}
        save_ok = save_ok && op::save_project(tmp_file("op_prop_fwd2.opj"), p, err);
        if (slurp_bytes(tmp_file("op_prop_fwd2.opj")) != fwd_bytes) redo_ok = false;
    }
    CHECK(save_ok, "prop.saves_ok");
    CHECK(undo_ok, "prop.undo_all_bytes_equal_original");
    CHECK(redo_ok, "prop.redo_all_bytes_equal_forward");
    std::printf("INFO ops property: %d sequences (len 1..40), %ld applied, %ld rejected\n",
                kSeqs, applied_total, rejected_total);
    CHECK(applied_total >= 2000, "prop.workload_exercises_many_applies");
}

// --- Document layer (WP-0141): open/save/dirty, undo/redo, one undo entry per gesture.
// src/doc/document.hpp; dirty is canonical save bytes vs the last open/save baseline.
static void test_document() {
    using nlohmann::json;
    std::string err;
    const std::string demo = std::string(OP_TEST_DATA) + "/fixtures/goldens/demo.opj";

    // Canonical save bytes of a Project — same writer as save_project, so equality with the
    // open-time baseline is exactly what dirty() compares. Never called on fixture paths.
    auto bytes_of = [](const op::Project& p, const std::string& path) {
        std::string serr;
        if (!op::save_project(path, p, serr)) return std::vector<uint8_t>{};
        return slurp_bytes(path);
    };

    // open of a missing file: false, err non-empty, document untouched.
    {
        op::Document d;
        CHECK(!op::Document::open(tmp_file("op_doc_missing_wp0141.opj"), d, err), "doc.open_missing_fails");
        CHECK(!err.empty(), "doc.open_missing_err_set");
        CHECK(d.revision() == 0 && d.path().empty() && !d.dirty() && !d.can_undo(),
              "doc.open_missing_unchanged");
    }

    // open of a loaded document failing must leave it exactly as it was.
    {
        op::Document d;
        CHECK(op::Document::open(demo, d, err), "doc.preopen_ok");
        CHECK(d.apply(json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -7.0}}, err) &&
                  d.project().tracks[0].gain_db == -7.0f,
              "doc.preopen_edit");
        CHECK(!op::Document::open(tmp_file("op_doc_missing_wp0141.opj"), d, err), "doc.open_fail_returns_false");
        CHECK(d.revision() == 1 && d.dirty() && d.can_undo() && d.path() == demo &&
                  d.project().tracks[0].gain_db == -7.0f,
              "doc.open_fail_leaves_doc_unchanged");
    }

    // open → path set, not dirty, revision 0, no history.
    op::Document doc;
    CHECK(op::Document::open(demo, doc, err), "doc.open_ok");
    CHECK(doc.path() == demo, "doc.open_path_set");
    CHECK(!doc.dirty(), "doc.open_not_dirty");
    CHECK(doc.revision() == 0, "doc.open_revision_zero");
    CHECK(!doc.can_undo() && !doc.can_redo(), "doc.open_no_history");
    const auto orig_bytes = bytes_of(doc.project(), tmp_file("op_doc_orig.opj"));

    // apply → dirty + can_undo + revision +1; undo → original bytes and clean again; redo → edited.
    CHECK(doc.apply(json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -3.5}}, err),
          "doc.apply_ok");
    CHECK(doc.dirty(), "doc.apply_dirty");
    CHECK(doc.can_undo() && !doc.can_redo(), "doc.apply_can_undo");
    CHECK(doc.revision() == 1, "doc.apply_revision_plus_1");
    const auto edited_bytes = bytes_of(doc.project(), tmp_file("op_doc_edited.opj"));
    CHECK(edited_bytes != orig_bytes, "doc.apply_changed_bytes");
    CHECK(doc.undo(), "doc.undo_ok");
    CHECK(bytes_of(doc.project(), tmp_file("op_doc_undo.opj")) == orig_bytes, "doc.undo_bytes_equal_original");
    CHECK(!doc.dirty(), "doc.undo_clean_again");  // undo back to saved state → clean by bytes
    CHECK(doc.can_redo() && !doc.can_undo(), "doc.undo_can_redo");
    CHECK(doc.redo(), "doc.redo_ok");
    CHECK(bytes_of(doc.project(), tmp_file("op_doc_redo.opj")) == edited_bytes, "doc.redo_bytes_equal_edited");

    // Rejected op: false + err, revision/bytes/history all unchanged.
    {
        const auto rev = doc.revision();
        const bool cu = doc.can_undo(), cr = doc.can_redo();
        const auto before = bytes_of(doc.project(), tmp_file("op_doc_rej_before.opj"));
        err.clear();
        CHECK(!doc.apply(json{{"op", "set_track_gain"}, {"index", 99}, {"gain_db", 3.0}}, err),
              "doc.reject_oob_op");
        CHECK(!err.empty(), "doc.reject_err_set");
        CHECK(doc.revision() == rev && doc.can_undo() == cu && doc.can_redo() == cr,
              "doc.reject_revision_history_unchanged");
        CHECK(bytes_of(doc.project(), tmp_file("op_doc_rej_after.opj")) == before, "doc.reject_bytes_unchanged");
    }

    // Gesture: 50 updates (−1 … −50) → exactly ONE undo entry; final gain −50; one undo restores.
    {
        op::Document g;
        CHECK(op::Document::open(demo, g, err), "doc.gesture_open_ok");
        const auto base_bytes = bytes_of(g.project(), tmp_file("op_doc_g_base.opj"));

        // No gesture active: update/end refused ("no gesture"), cancel is a no-op.
        err.clear();
        CHECK(!g.update_gesture(json{{"op", "set_bpm"}, {"bpm", 100.0}}, err) && err == "no gesture",
              "doc.update_without_gesture");

        CHECK(g.begin_gesture(), "doc.begin_gesture_ok");
        CHECK(g.gesture_active(), "doc.gesture_active_flag");
        CHECK(!g.begin_gesture(), "doc.begin_twice_fails");

        // apply/undo/redo refused during a gesture; state and revision untouched.
        const auto rev0 = g.revision();
        err.clear();
        CHECK(!g.apply(json{{"op", "set_bpm"}, {"bpm", 100.0}}, err) && err == "gesture active",
              "doc.apply_refused_during_gesture");
        CHECK(!g.undo() && !g.redo(), "doc.undo_redo_refused_during_gesture");
        CHECK(g.revision() == rev0, "doc.refusals_keep_revision_and_state");

        // Invalid preview op: false, preview unchanged, no revision bump.
        CHECK(!g.update_gesture(json{{"op", "set_track_gain"}, {"index", 99}, {"gain_db", 1.0}}, err),
              "doc.update_rejects_bad_op");
        CHECK(g.project().tracks[0].gain_db == -12.0f && g.revision() == rev0,
              "doc.bad_update_preview_unchanged");

        bool updates_ok = true;
        for (int i = 1; i <= 50 && updates_ok; ++i)
            updates_ok = g.update_gesture(
                json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -static_cast<double>(i)}}, err);
        CHECK(updates_ok, "doc.fifty_updates_ok");
        CHECK(g.project().tracks[0].gain_db == -50.0f, "doc.gesture_preview_minus50");
        CHECK(!g.can_undo() && !g.can_redo(), "doc.gesture_previews_touch_no_history");
        CHECK(g.revision() == rev0 + 50, "doc.gesture_revision_per_update");

        CHECK(g.end_gesture(err), "doc.end_gesture_ok");
        CHECK(!g.gesture_active(), "doc.gesture_ended");
        CHECK(g.project().tracks[0].gain_db == -50.0f && g.revision() == rev0 + 50,
              "doc.end_keeps_last_preview");  // commit == last preview → no extra revision bump
        CHECK(g.can_undo() && !g.can_redo(), "doc.end_gesture_one_entry");
        CHECK(g.dirty(), "doc.end_gesture_dirty");
        CHECK(g.undo(), "doc.gesture_single_undo");
        CHECK(bytes_of(g.project(), tmp_file("op_doc_g_undo.opj")) == base_bytes,
              "doc.gesture_undo_restores_original_bytes");
        CHECK(!g.can_undo() && !g.undo(), "doc.gesture_exactly_one_entry");
        CHECK(!g.dirty(), "doc.gesture_undo_clean");
    }

    // Updates are NOT cumulative: −6 then −3 previews/commits −3, not −9.
    {
        op::Document g;
        CHECK(op::Document::open(demo, g, err), "doc.nc_open_ok");
        CHECK(g.begin_gesture(), "doc.nc_begin");
        CHECK(g.update_gesture(json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -6.0}}, err) &&
                  g.project().tracks[0].gain_db == -6.0f,
              "doc.nc_update_minus6");
        CHECK(g.update_gesture(json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -3.0}}, err) &&
                  g.project().tracks[0].gain_db == -3.0f,
              "doc.nc_not_cumulative");  // −3 from the begin state, not −9
        CHECK(g.end_gesture(err), "doc.nc_end_ok");
        CHECK(g.project().tracks[0].gain_db == -3.0f && g.can_undo(), "doc.nc_commit_is_last_update");
    }

    // cancel_gesture: restores the begin state, adds no history entry, ends the gesture.
    {
        op::Document g;
        CHECK(op::Document::open(demo, g, err), "doc.cancel_open_ok");
        const auto base_bytes = bytes_of(g.project(), tmp_file("op_doc_c_base.opj"));
        CHECK(g.begin_gesture(), "doc.cancel_begin");
        CHECK(g.update_gesture(json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -20.0}}, err),
              "doc.cancel_update_ok");
        const auto rev_preview = g.revision();  // preview −20 already bumped the revision
        g.cancel_gesture();
        CHECK(!g.gesture_active(), "doc.cancel_ends_gesture");
        CHECK(bytes_of(g.project(), tmp_file("op_doc_c_after.opj")) == base_bytes, "doc.cancel_restores");
        CHECK(!g.can_undo() && !g.dirty(), "doc.cancel_no_entry_clean");
        CHECK(g.revision() == rev_preview + 1, "doc.cancel_revision_bump");  // restore changed project()
    }

    // save_as: path moves, file bytes == op::save_project of project(), document clean; then
    // edit → dirty → save() writes the new path and does not touch history or revision.
    {
        op::Document s;
        CHECK(op::Document::open(demo, s, err), "doc.saveas_open_ok");
        CHECK(s.apply(json{{"op", "set_track_gain"}, {"index", 1}, {"gain_db", -9.0}}, err) && s.dirty(),
              "doc.saveas_dirty_pre");
        CHECK(s.save_as(tmp_file("op_doc_saveas.opj"), err), "doc.saveas_ok");
        CHECK(s.path() == tmp_file("op_doc_saveas.opj"), "doc.saveas_path_set");
        CHECK(!s.dirty(), "doc.saveas_clean");
        CHECK(op::save_project(tmp_file("op_doc_saveref.opj"), s.project(), err), "doc.saveref_ok");
        CHECK(slurp_bytes(tmp_file("op_doc_saveas.opj")) == slurp_bytes(tmp_file("op_doc_saveref.opj")),
              "doc.saveas_bytes_match_save_project");
        const auto rev = s.revision();
        CHECK(s.apply(json{{"op", "set_track_gain"}, {"index", 1}, {"gain_db", -8.0}}, err) && s.dirty(),
              "doc.save_dirty_pre");
        CHECK(s.save(err), "doc.save_ok");
        CHECK(!s.dirty() && s.revision() == rev + 1, "doc.save_clean_revision_untouched_by_save");
        CHECK(op::save_project(tmp_file("op_doc_saveref.opj"), s.project(), err) &&
                  slurp_bytes(tmp_file("op_doc_saveas.opj")) == slurp_bytes(tmp_file("op_doc_saveref.opj")),
              "doc.save_wrote_new_path");  // save() overwrote the save_as path with current bytes
    }

    // track_rows(): derived rows reflect name/gain/pan/mute/solo and audible per WP-0136 rule.
    {
        op::Document t;
        CHECK(op::Document::open(demo, t, err), "doc.rows_open_ok");
        auto rows = t.track_rows();
        CHECK(rows.size() == 3, "doc.rows_count");
        CHECK(rows[0].name == "Metronome" && rows[1].name == "Bassline" && rows[2].name == "Kick",
              "doc.rows_names");
        CHECK(rows[0].gain_db == -12.0f && std::fabs(rows[1].pan + 0.3f) < 1e-5f &&
                  rows[2].gain_db == -2.0f,
              "doc.rows_values");
        CHECK(!rows[0].mute && !rows[0].solo && rows[0].audible && rows[1].audible && rows[2].audible,
              "doc.rows_default_audible");
        CHECK(t.apply(json{{"op", "set_track_solo"}, {"index", 2}, {"solo", true}}, err) &&
                  t.apply(json{{"op", "set_track_mute"}, {"index", 0}, {"mute", true}}, err),
              "doc.rows_mixer_ops_ok");
        rows = t.track_rows();
        CHECK(rows[2].solo && !rows[2].mute && rows[2].audible, "doc.rows_solo_audible");
        CHECK(rows[0].mute && !rows[0].audible, "doc.rows_muted_inaudible");
        CHECK(!rows[1].solo && !rows[1].audible, "doc.rows_solo_gates_others");
        CHECK(t.apply(json{{"op", "set_track_mute"}, {"index", 2}, {"mute", true}}, err),
              "doc.rows_mute_solo_ok");
        rows = t.track_rows();
        CHECK(rows[2].solo && rows[2].mute && !rows[2].audible, "doc.rows_mute_wins_over_solo");
    }
}

// ---------------------------------------------------------------- autosave + recovery (WP-0157)

static std::string slurp_str(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void test_document_autosave() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    std::string err;
    const std::string demo = std::string(OP_TEST_DATA) + "/fixtures/goldens/demo.opj";
    const fs::path dir = fs::path(tmp_file("op_doc_autosave_wp0157"));
    fs::create_directories(dir);
    const std::string main_path = (dir / "song.opj").string();
    const std::string auto_path = op::Document::autosave_path(main_path);
    const std::string tmp_path = auto_path + ".tmp";
    const auto fresh_copy = [&] {
        fs::remove(auto_path);
        fs::remove(tmp_path);
        fs::copy_file(demo, main_path, fs::copy_options::overwrite_existing);
    };
    const json edit_a{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -3.5}};
    const json edit_b{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -8.25}};

    // 1. untitled: exact error, nothing written even though dirty.
    {
        op::Document u;
        CHECK(u.apply(json{{"op", "add_track"},
                           {"track", json{{"gain_db", -6.0}, {"name", "Pad"}, {"pan", 0.0},
                                          {"voice", json{{"kind", "metronome"}, {"accent_every", 4}}}}}},
                      err) &&
                  u.dirty(),
              "doc.autosave.untitled_dirty_pre");
        err.clear();
        CHECK(!u.autosave(err) && err == "cannot autosave an untitled document", "doc.autosave.untitled_error");
        CHECK(!fs::exists(op::Document::autosave_path("")), "doc.autosave.untitled_no_file");
    }

    // 2. clean document: no autosave file created, err unchanged.
    fresh_copy();
    op::Document doc;
    CHECK(op::Document::open(main_path, doc, err), "doc.autosave.open_ok");
    err = "keep";
    CHECK(doc.autosave(err) && err == "keep" && !fs::exists(auto_path), "doc.autosave.clean_no_file");

    // 3. dirty: autosave file == project_bytes(project()), no .tmp left, main file untouched.
    const std::string main_before = slurp_str(main_path);
    CHECK(doc.apply(edit_a, err), "doc.autosave.edit_a");
    CHECK(doc.autosave(err) && fs::exists(auto_path), "doc.autosave.dirty_writes");
    CHECK(slurp_str(auto_path) == op::project_bytes(doc.project()), "doc.autosave.bytes_equal_project_bytes");
    CHECK(!fs::exists(tmp_path), "doc.autosave.no_tmp_left");
    CHECK(slurp_str(main_path) == main_before, "doc.autosave.main_bytes_unchanged");
    CHECK(doc.dirty() && doc.revision() == 1 && doc.path() == main_path, "doc.autosave.state_unchanged");

    // 4. same revision: second call does not rewrite.
    fs::remove(auto_path);
    CHECK(doc.autosave(err) && !fs::exists(auto_path), "doc.autosave.same_revision_no_rewrite");

    // 5. new revision: rewritten with the new content.
    CHECK(doc.apply(edit_b, err) && doc.autosave(err), "doc.autosave.new_revision_ok");
    CHECK(slurp_str(auto_path) == op::project_bytes(doc.project()), "doc.autosave.new_revision_rewritten");

    // 6. failure: <autosave>.tmp is a directory -> false, exact error, main and prior autosave untouched.
    const std::string prior_auto = slurp_str(auto_path);
    CHECK(doc.apply(edit_a, err), "doc.autosave.edit_for_tmp_dir");
    fs::create_directory(tmp_path);
    err.clear();
    CHECK(!doc.autosave(err) && err == "cannot write autosave '" + auto_path + "'",
          "doc.autosave.tmp_dir_error");
    CHECK(slurp_str(auto_path) == prior_auto, "doc.autosave.tmp_dir_prior_untouched");
    CHECK(slurp_str(main_path) == main_before, "doc.autosave.tmp_dir_main_untouched");
    fs::remove_all(tmp_path);

    // 7. failure: the autosave path itself is a directory -> false, no .tmp left, and the failed
    //    revision is not recorded (a retry after removing the directory writes).
    fresh_copy();
    CHECK(op::Document::open(main_path, doc, err) && doc.apply(edit_a, err), "doc.autosave.dir_target_setup");
    fs::create_directory(auto_path);
    err.clear();
    CHECK(!doc.autosave(err) && err == "cannot write autosave '" + auto_path + "'",
          "doc.autosave.dir_target_error");
    CHECK(!fs::exists(tmp_path), "doc.autosave.dir_target_no_tmp");
    fs::remove(auto_path);
    CHECK(doc.autosave(err) && fs::exists(auto_path), "doc.autosave.retry_after_failure_writes");

    // 8. save() removes the autosave file and its .tmp.
    CHECK(doc.save(err) && !fs::exists(auto_path) && !fs::exists(tmp_path), "doc.autosave.save_removes_autosave");
    CHECK(!doc.dirty(), "doc.autosave.save_clean");

    // 9. save_as() removes the new path's autosave file, not the old path's.
    fresh_copy();
    const std::string other = (dir / "other.opj").string();
    const std::string other_auto = op::Document::autosave_path(other);
    fs::remove(other_auto);
    CHECK(op::Document::open(main_path, doc, err) && doc.apply(edit_a, err) && doc.autosave(err),
          "doc.autosave.saveas_setup");
    CHECK(!fs::exists(other_auto), "doc.autosave.saveas_other_absent_pre");
    fs::copy_file(auto_path, other_auto, fs::copy_options::overwrite_existing);  // stale autosave of "other"
    CHECK(doc.save_as(other, err) && !fs::exists(other_auto),
          "doc.autosave.saveas_removes_new_path_autosave");
    CHECK(fs::exists(auto_path), "doc.autosave.saveas_old_path_autosave_kept");

    // 10. recovery_state: None without autosave, None for identical content, None for corrupt autosave.
    fresh_copy();
    CHECK(op::Document::recovery_state(main_path) == op::RecoveryState::None, "doc.autosave.recovery_none_no_file");
    fs::copy_file(main_path, auto_path, fs::copy_options::overwrite_existing);
    CHECK(op::Document::recovery_state(main_path) == op::RecoveryState::None, "doc.autosave.recovery_none_identical");
    write_text(auto_path, "{ \"truncated\": ");
    CHECK(op::Document::recovery_state(main_path) == op::RecoveryState::None, "doc.autosave.recovery_none_corrupt");
    CHECK(op::Document::recovery_state(main_path + ".missing") == op::RecoveryState::None,
          "doc.autosave.recovery_none_missing_main");

    // 11. recovery_state AutosaveNewer for differing content; plain open ignores it and leaves it.
    fresh_copy();
    CHECK(op::Document::open(main_path, doc, err) && doc.apply(edit_b, err) && doc.autosave(err),
          "doc.autosave.recover_setup");
    CHECK(op::Document::recovery_state(main_path) == op::RecoveryState::AutosaveNewer,
          "doc.autosave.recovery_newer");
    op::Document plain;
    CHECK(op::Document::open(main_path, plain, err) && !plain.dirty() && fs::exists(auto_path),
          "doc.autosave.plain_open_ignores_autosave");

    // 12. open_recovered: dirty, content == autosave, then save() makes the main file the recovered content.
    const std::string recovered_bytes = slurp_str(auto_path);
    op::Document rec;
    CHECK(op::Document::open_recovered(main_path, rec, err), "doc.autosave.open_recovered_ok");
    CHECK(rec.dirty() && rec.path() == main_path && rec.revision() == 0 && !rec.can_undo(),
          "doc.autosave.open_recovered_dirty_state");
    CHECK(op::project_bytes(rec.project()) == recovered_bytes, "doc.autosave.open_recovered_content");
    CHECK(slurp_str(main_path) != recovered_bytes && fs::exists(auto_path),
          "doc.autosave.open_recovered_files_untouched");
    CHECK(rec.save(err) && slurp_str(main_path) == recovered_bytes && !rec.dirty(),
          "doc.autosave.recovered_save_writes_main");

    // 13. open_recovered with no recoverable autosave: false, exact error, out unchanged.
    fresh_copy();
    op::Document keep;
    CHECK(op::Document::open(main_path, keep, err) && keep.apply(edit_a, err), "doc.autosave.keep_setup");
    err.clear();
    CHECK(!op::Document::open_recovered(main_path, keep, err) &&
              err == "no recoverable autosave for '" + main_path + "'",
          "doc.autosave.open_recovered_none_error");
    CHECK(keep.revision() == 1 && keep.dirty() && keep.path() == main_path && keep.can_undo() &&
              keep.project().tracks[0].gain_db == -3.5f,
          "doc.autosave.open_recovered_none_out_unchanged");

    fs::remove_all(dir);
}

// ---------------------------------------------------------------- resampler (WP-0138)

// Minimal mono PCM16 WAV writer — tests need assets at arbitrary rates; wav::write is stereo-only.
static void write_mono_pcm16(const std::string& path, int sr, const std::vector<float>& s) {
    auto le32 = [](std::string& b, uint32_t v) {
        for (int i = 0; i < 4; ++i) b += static_cast<char>((v >> (8 * i)) & 0xFFu);
    };
    auto le16 = [](std::string& b, uint16_t v) {
        b += static_cast<char>(v & 0xFFu); b += static_cast<char>((v >> 8) & 0xFFu);
    };
    const uint32_t data_bytes = static_cast<uint32_t>(s.size()) * 2u;
    std::string h;
    h += "RIFF"; le32(h, 36 + data_bytes); h += "WAVE";
    h += "fmt "; le32(h, 16); le16(h, 1); le16(h, 1);  // PCM, mono
    le32(h, static_cast<uint32_t>(sr)); le32(h, static_cast<uint32_t>(sr) * 2u); le16(h, 2); le16(h, 16);
    h += "data"; le32(h, data_bytes);
    for (float x : s) le16(h, static_cast<uint16_t>(op::wav::float_to_pcm16(x)));
    write_text(path, h);
}

// One-bar 48 kHz project with a single hard-left (gain exactly 1.0), full-gain sampler note.
static op::Project sampler_project_1bar(const std::string& wav_rel) {
    op::Project p;
    p.bpm = 120.0; p.length_bars = 1.0; p.beats_per_bar = 4; p.sample_rate = 48000;
    op::Track tk; tk.name = "S"; tk.gain_db = 0.0f; tk.pan = -1.0f;
    tk.voice.kind = op::VoiceKind::Sampler; tk.voice.path = wav_rel;
    p.tracks.push_back(tk);
    op::Pattern pat; pat.track = 0; pat.notes.push_back({0.0, 4.0});
    p.patterns.push_back(pat);
    return p;
}

static void test_resample() {
    std::string err;

    // Rate policy (spec): 8000–192000 Hz inclusive.
    CHECK(op::resample::rate_supported(8000) && op::resample::rate_supported(44100) &&
          op::resample::rate_supported(48000) && op::resample::rate_supported(192000),
          "resample.rate_bounds_inclusive");
    CHECK(!op::resample::rate_supported(7999) && !op::resample::rate_supported(192001),
          "resample.rate_out_of_range_rejected");

    // (a) Native-rate assets take the untouched path — bit-identical to the loader output.
    std::vector<float> s(480);
    for (size_t i = 0; i < s.size(); ++i)
        s[i] = 0.5f * op::dsp::sinf_(0.13f * static_cast<float>(i)) +
               0.25f * op::dsp::cosf_(0.71f * static_cast<float>(i));
    write_mono_pcm16(tmp_file("op_rs_native.wav"), 48000, s);
    {
        const op::Project p = sampler_project_1bar("op_rs_native.wav");
        op::wav::StereoBuffer buf;
        CHECK(op::render_project(p, tmp_dir(), buf, err), "resample.native_mono_renders");
        const op::wav::ReadResult rr = op::wav::read_wav(tmp_file("op_rs_native.wav"));
        bool bitid = rr.ok && buf.frames() == 96000;
        for (size_t n = 0; bitid && n < s.size(); ++n) {
            if (buf.interleaved[n * 2] != rr.samples[n]) bitid = false;   // exact float equality
            if (buf.interleaved[n * 2 + 1] != 0.0f) bitid = false;        // hard-left: right silent
        }
        CHECK(bitid, "resample.native_48k_bit_identical_to_loader");
    }
    {
        op::wav::StereoBuffer sb;
        sb.sample_rate = 48000;
        sb.interleaved.resize(s.size() * 2);
        for (size_t i = 0; i < s.size(); ++i) {
            sb.interleaved[i * 2] = s[i];
            sb.interleaved[i * 2 + 1] = -0.3f * op::dsp::sinf_(0.91f * static_cast<float>(i));
        }
        CHECK(op::wav::write_pcm16_stereo(tmp_file("op_rs_native_st.wav"), sb, err), "resample.native_stereo_written");
        const op::Project p = sampler_project_1bar("op_rs_native_st.wav");
        op::wav::StereoBuffer buf;
        CHECK(op::render_project(p, tmp_dir(), buf, err), "resample.native_stereo_renders");
        const op::wav::ReadResult rr = op::wav::read_wav(tmp_file("op_rs_native_st.wav"));
        bool bitid = rr.ok && rr.channels == 2;
        for (size_t n = 0; bitid && n < s.size(); ++n) {
            if (buf.interleaved[n * 2] != rr.samples[n * 2]) bitid = false;
            if (buf.interleaved[n * 2 + 1] != 0.0f) bitid = false;
        }
        CHECK(bitid, "resample.native_48k_stereo_bit_identical_to_loader");
    }

    // (b) −6 dBFS 1 kHz sine generated at 44100 Hz, 2 s → resampled to 48000.
    const double amp = std::pow(10.0, -6.0 / 20.0);
    std::vector<float> x(2 * 44100);
    for (size_t n = 0; n < x.size(); ++n)
        x[n] = static_cast<float>(amp * std::sin(2.0 * op::dsp::kPi_d() * 1000.0 *
                                                  static_cast<double>(n) / 44100.0));
    std::vector<float> y;
    CHECK(op::resample::windowsinc(x, 1, 44100, 48000, y), "resample.b_call_ok");
    // (d) exact length rule: ceil(88200 × 48000 / 44100) = 96000.
    CHECK(y.size() == 96000, "resample.out_len_441_to_48_exact");

    // Frequency by zero-crossing count over the middle 1.5 s, with linear-interpolated crossing
    // times (naive integer counting has ±1/(2T) ≈ ±0.33 Hz boundary error, larger than the spec).
    std::vector<double> tau;
    for (long long k = 12000; k < 84000 - 1; ++k) {
        const double a = y[k], b = y[k + 1];
        if ((a < 0.0) != (b < 0.0)) tau.push_back(static_cast<double>(k) + a / (a - b));
    }
    const double fhat = (static_cast<double>(tau.size()) - 1.0) / 2.0 /
                        ((tau.back() - tau.front()) / 48000.0);
    std::printf("INFO resample freq_est=%.6f Hz err=%.6f Hz\n", fhat, std::fabs(fhat - 1000.0));
    CHECK(std::fabs(fhat - 1000.0) <= 0.2, "resample.b_freq_within_0p2_hz");

    // SNR vs ideal 48 kHz sine with best-fit phase (2×2 least squares on sin/cos), ignoring the
    // first and last 64 output samples (spec).
    double scc = 0, sss = 0, scs = 0, bc = 0, bs = 0;
    for (long long k = 64; k < 96000 - 64; ++k) {
        const double th = 2.0 * op::dsp::kPi_d() * 1000.0 * static_cast<double>(k) / 48000.0;
        const double c = std::cos(th), sn = std::sin(th);
        scc += c * c; sss += sn * sn; scs += sn * c; bc += y[k] * c; bs += y[k] * sn;
    }
    const double det = scc * sss - scs * scs;
    const double alpha = (bc * sss - bs * scs) / det, beta = (bs * scc - bc * scs) / det;
    const double proj = alpha * bc + beta * bs;   // ‖projection‖² via normal equations
    // Two-pass residual: tot − proj loses all significant digits in double once SNR ≳ 135 dB
    // (float32 output noise floor), so sum the squared fit errors directly.
    double resid = 0;
    for (long long k = 64; k < 96000 - 64; ++k) {
        const double th = 2.0 * op::dsp::kPi_d() * 1000.0 * static_cast<double>(k) / 48000.0;
        const double e = y[k] - (alpha * std::cos(th) + beta * std::sin(th));
        resid += e * e;
    }
    const double snr_db = (resid > 0.0) ? 10.0 * std::log10(proj / resid) : 999.0;
    std::printf("INFO resample snr_vs_ideal_sine=%.2f dB\n", snr_db);
    CHECK(snr_db >= 70.0, "resample.b_snr_ge_70db");

    // Stereo converter sanity: identical tone on both channels → exactly the mono result.
    std::vector<float> xs(x.size() * 2);
    for (size_t n = 0; n < x.size(); ++n) { xs[n * 2] = x[n]; xs[n * 2 + 1] = x[n]; }
    std::vector<float> ys;
    CHECK(op::resample::windowsinc(xs, 2, 44100, 48000, ys), "resample.stereo_call_ok");
    bool chan_eq = ys.size() == y.size() * 2;
    for (size_t k = 0; chan_eq && k < y.size(); ++k)
        if (ys[k * 2] != y[k] || ys[k * 2 + 1] != y[k]) chan_eq = false;
    CHECK(chan_eq, "resample.stereo_channels_match_mono_bitwise");

    // (c) 96000 → 48000 with a 30 kHz tone (above output Nyquist): aliasing must be rejected.
    // The tone is Hann-ramped in/out over 50 ms so the full-output RMS measures the filter's
    // stopband rejection; an abrupt onset injects broadband click energy that partial windows
    // at the signal edges normalize straight through, which is a measurement artifact, not aliasing.
    std::vector<float> x96(96000);
    {
        const double ramp = 2400.0;  // 50 ms half-cosine fades
        for (size_t n = 0; n < x96.size(); ++n) {
            double e = 1.0;
            if (static_cast<double>(n) < ramp)
                e = 0.5 - 0.5 * std::cos(op::dsp::kPi_d() * static_cast<double>(n) / ramp);
            else if (n + 1 >= x96.size() - static_cast<size_t>(ramp))
                e = 0.5 - 0.5 * std::cos(op::dsp::kPi_d() *
                                         static_cast<double>(x96.size() - 1 - n) / ramp);
            x96[n] = static_cast<float>(amp * e * std::sin(2.0 * op::dsp::kPi_d() * 30000.0 *
                                                            static_cast<double>(n) / 96000.0));
        }
    }
    std::vector<float> y48;
    CHECK(op::resample::windowsinc(x96, 1, 96000, 48000, y48), "resample.c_call_ok");
    CHECK(y48.size() == 48000, "resample.out_len_96k_to_48k_exact");
    {
        auto rms = [](const std::vector<float>& v) {
            double acc = 0;
            for (float f : v) acc += static_cast<double>(f) * static_cast<double>(f);
            return std::sqrt(acc / static_cast<double>(v.size()));
        };
        const double ratio_db = 20.0 * std::log10(rms(y48) / rms(x96));
        std::printf("INFO resample alias_30k_rms=%.2f dB rel input\n", ratio_db);
        CHECK(ratio_db <= -60.0, "resample.c_alias_rejected_le_minus60db");
    }

    // (d) exact ceil rule on non-dividing frame counts.
    std::vector<float> xk(1000);
    for (size_t n = 0; n < xk.size(); ++n)
        xk[n] = static_cast<float>(std::sin(2.0 * op::dsp::kPi_d() * 440.0 * static_cast<double>(n) / 44100.0));
    std::vector<float> yk;
    CHECK(op::resample::windowsinc(xk, 1, 44100, 48000, yk) && yk.size() == 1089,
          "resample.out_len_ceil_rule_1000f_441_to_48");   // ceil(1088.435…) = 1089
    std::vector<float> xj(96001, 0.0f);
    std::vector<float> yj;
    CHECK(op::resample::windowsinc(xj, 1, 96000, 48000, yj) && yj.size() == 48001,
          "resample.out_len_ceil_rule_96001f_to_48001");   // ceil(48000.5) = 48001

    // (e) Unsupported rate 4000 Hz → render fails with the spec'd message (CLI exit 5).
    std::vector<float> slow = {0.0f, 0.25f, -0.25f, 0.1f};
    write_mono_pcm16(tmp_file("op_rs_lowrate.wav"), 4000, slow);
    const op::Project pe = sampler_project_1bar("op_rs_lowrate.wav");
    op::wav::StereoBuffer be;
    CHECK(!op::render_project(pe, tmp_dir(), be, err), "resample.e_unsupported_rate_fails_render");
    CHECK(err == "sampler: unsupported sample rate 4000", "resample.e_error_message_exact");
}

// WP-0142: op::project_bytes is the single definition of the file format. save_project and
// Document must write exactly these bytes — no second serializer anywhere.
static void test_project_bytes() {
    std::string err;

    // Every golden: load → save_project to /tmp → read back binary == op::project_bytes(loaded).
    // Some goldens are older, non-canonical formatting on disk, so the comparison is against what
    // save_project writes now — never against the fixture file itself.
    const char* names[] = {"arranged", "demo",     "imported", "mixer",
                           "panorama", "pitched",  "resampled", "tempo"};
    for (const char* name : names) {
        const std::string fix = std::string(OP_TEST_DATA) + "/fixtures/goldens/" + name + ".opj";
        op::Project p;
        bool ok = op::load_project(fix, p, err);
        const std::string tmp = tmp_file(std::string("op_projbytes_") + name + ".opj");
        if (ok) ok = op::save_project(tmp, p, err);
        if (ok) {
            const std::string want = op::project_bytes(p);
            ok = !want.empty() &&
                 slurp_bytes(tmp) == std::vector<uint8_t>(want.begin(), want.end());
        }
        CHECK(ok, (std::string("projbytes.") + name + "_matches_save").c_str());
    }

    // Default project: non-empty, exactly one trailing '\n', identical across calls.
    {
        const op::Project p;
        const std::string b = op::project_bytes(p);
        CHECK(!b.empty(), "projbytes.default_nonempty");
        CHECK(b.back() == '\n', "projbytes.default_ends_with_newline");
        CHECK(op::project_bytes(p) == b, "projbytes.stable_across_calls");
    }

    // Document agrees: save_as writes exactly op::project_bytes(doc.project()).
    {
        const std::string demo = std::string(OP_TEST_DATA) + "/fixtures/goldens/demo.opj";
        op::Document doc;
        bool ok = op::Document::open(demo, doc, err);
        if (ok) ok = doc.save_as(tmp_file("op_projbytes_doc.opj"), err);
        if (ok) {
            const std::string want = op::project_bytes(doc.project());
            ok = slurp_bytes(tmp_file("op_projbytes_doc.opj")) ==
                 std::vector<uint8_t>(want.begin(), want.end());
        }
        CHECK(ok, "projbytes.document_save_as_agrees");
    }
}

// --- WP-0144: objective audio features (docs/spec/analyze.md) ------------------------------------

// Test hook defined in analyze.cpp (deliberately not part of the public header).
namespace op::analyze { void k_weight_coefficients(int fs, double out[10]); }

static std::vector<float> make_sine_stereo(double amp, double freq_hz, long long frames, double fs) {
    std::vector<float> x(static_cast<size_t>(frames) * 2u);
    for (long long i = 0; i < frames; ++i) {
        const double s = amp * op::dsp::sind_(2.0 * op::dsp::kPi_d() * freq_hz *
                                              static_cast<double>(i) / fs);
        x[static_cast<size_t>(i) * 2u] = static_cast<float>(s);
        x[static_cast<size_t>(i) * 2u + 1u] = static_cast<float>(s);
    }
    return x;
}

static bool near_(double got, double want, double tol) { return got >= want - tol && got <= want + tol; }

static void test_analyze() {
    using op::analyze::analyze_samples;
    using op::analyze::to_json;
    const int fs = 48000;

    // K-weighting coefficients at 48 kHz must reproduce the ITU-R BS.1770-4 table values to 1e-9.
    double c[10];
    op::analyze::k_weight_coefficients(48000, c);
    CHECK(near_(c[0], 1.53512485958697, 1e-9) && near_(c[1], -2.69169618940638, 1e-9) &&
          near_(c[2], 1.19839281085285, 1e-9) && near_(c[3], -1.69065929318241, 1e-9) &&
          near_(c[4], 0.73248077421585, 1e-9), "analyze.k_weight_stage1_48k_matches_bs1770");
    CHECK(c[5] == 1.0 && c[6] == -2.0 && c[7] == 1.0 &&
          near_(c[8], -1.99004745483398, 1e-9) && near_(c[9], 0.99007225036621, 1e-9),
          "analyze.k_weight_stage2_48k_matches_bs1770");

    // Sine peak value exact: amplitude 0.5 → peak_dbfs −6.0206 ± 1e-4 (per channel and overall).
    auto half = make_sine_stereo(0.5, 1000.0, 48000, 48000.0);
    auto fh = analyze_samples(half, 2, fs);
    CHECK(fh.peak_dbfs && near_(*fh.peak_dbfs, -6.0206, 1e-4) &&
          fh.peak_dbfs_per_channel.size() == 2 && fh.peak_dbfs_per_channel[0] &&
          fh.peak_dbfs_per_channel[1] && near_(*fh.peak_dbfs_per_channel[0], -6.0206, 1e-4) &&
          near_(*fh.peak_dbfs_per_channel[1], -6.0206, 1e-4), "analyze.sine_peak_amplitude_half_is_minus6_0206");

    // BS.1770 calibration tone: stereo 1 kHz sine at −23 dBFS (10 s, 48 kHz) → −23.0 LUFS ± 0.1.
    auto s23 = make_sine_stereo(op::dsp::powd_(10.0, -23.0 / 20.0), 1000.0, 480000, 48000.0);
    auto f23 = analyze_samples(s23, 2, fs);
    CHECK(f23.integrated_lufs && near_(*f23.integrated_lufs, -23.0, 0.1), "analyze.lufs_stereo_1k_minus23dBFS");

    // Same at −33 dBFS → −33.0 LUFS ± 0.1 (still above the absolute gate; relative gate keeps all).
    auto s33 = make_sine_stereo(op::dsp::powd_(10.0, -33.0 / 20.0), 1000.0, 480000, 48000.0);
    auto f33 = analyze_samples(s33, 2, fs);
    CHECK(f33.integrated_lufs && near_(*f33.integrated_lufs, -33.0, 0.1), "analyze.lufs_stereo_1k_minus33dBFS");

    // Spectral centroid of a pure 1 kHz tone (48 kHz) within ±5 Hz of 1000.
    CHECK(f23.spectral_centroid_hz && near_(*f23.spectral_centroid_hz, 1000.0, 5.0), "analyze.centroid_1k_tone_within_5hz");

    // Digital silence (1 s stereo zeros): every optional feature is nullopt...
    std::vector<float> z(96000, 0.0f);
    auto fz = analyze_samples(z, 2, fs);
    CHECK(!fz.peak_dbfs && !fz.rms_dbfs && !fz.integrated_lufs && !fz.spectral_centroid_hz &&
          fz.peak_dbfs_per_channel.size() == 2 && !fz.peak_dbfs_per_channel[0] &&
          !fz.peak_dbfs_per_channel[1], "analyze.silence_every_optional_is_nullopt");
    // ...and to_json prints null for each of them.
    CHECK(to_json(fz) == "{\n"
                         "  \"sample_rate\": 48000,\n"
                         "  \"channels\": 2,\n"
                         "  \"frames\": 48000,\n"
                         "  \"duration_s\": 1.000000,\n"
                         "  \"peak_dbfs\": null,\n"
                         "  \"peak_dbfs_per_channel\": [null, null],\n"
                         "  \"rms_dbfs\": null,\n"
                         "  \"integrated_lufs\": null,\n"
                         "  \"spectral_centroid_hz\": null\n"
                         "}\n", "analyze.silence_json_prints_null");

    // to_json of a fixed Features matches the spec sample text character for character.
    op::analyze::Features f;
    f.sample_rate = 48000; f.channels = 2; f.frames = 480000; f.duration_s = 10.0;
    f.peak_dbfs = -6.0206;
    f.peak_dbfs_per_channel = { -6.0206, -9.0309 };
    f.rms_dbfs = -23.0103; f.integrated_lufs = -23.0021; f.spectral_centroid_hz = 1000.1234;
    CHECK(to_json(f) == "{\n"
                        "  \"sample_rate\": 48000,\n"
                        "  \"channels\": 2,\n"
                        "  \"frames\": 480000,\n"
                        "  \"duration_s\": 10.000000,\n"
                        "  \"peak_dbfs\": -6.0206,\n"
                        "  \"peak_dbfs_per_channel\": [-6.0206, -9.0309],\n"
                        "  \"rms_dbfs\": -23.0103,\n"
                        "  \"integrated_lufs\": -23.0021,\n"
                        "  \"spectral_centroid_hz\": 1000.1234\n"
                        "}\n", "analyze.to_json_matches_spec_sample_byte_exact");

    // Determinism: analyzing the same buffer twice yields byte-identical JSON.
    CHECK(to_json(analyze_samples(s23, 2, fs)) == to_json(f23), "analyze.repeat_analysis_is_byte_identical");
}

// ---- opd diff (WP-0146): diff_projects, docs/spec/operations.md §opd diff ----
static std::string journal_text(const std::vector<nlohmann::json>& ops) {
    std::string s;
    for (const auto& o : ops) s += o.dump() + "\n";
    return s;
}

// Hand-built project: 3 tracks, 3 patterns (one per track), 3 clips (one per pattern).
static op::Project diff_base_project() {
    op::Project p;
    p.bpm = 120.0; p.length_bars = 4.0; p.sample_rate = 48000; p.seed = 5;
    op::Track t0; t0.name = "Sine"; t0.voice.kind = op::VoiceKind::Sine; t0.voice.freq_hz = 220.0f;
    op::Track t1; t1.name = "Metro"; t1.voice.kind = op::VoiceKind::Metronome;
    op::Track t2; t2.name = "Sine2"; t2.voice.kind = op::VoiceKind::Sine; t2.voice.freq_hz = 330.0f;
    p.tracks = {t0, t1, t2};
    op::Pattern p0; p0.track = 0; p0.notes.push_back({0.0, 0.25});
    op::Pattern p1; p1.track = 1; p1.notes.push_back({0.0, 1.0});
    op::Pattern p2; p2.track = 2; p2.notes.push_back({0.5, 0.5});
    p.patterns = {p0, p1, p2};
    p.arrangement = {{0, 0.0, 1, 0.0}, {1, 1.0, 1, 0.0}, {2, 2.0, 1, 0.0}};
    return p;
}

// Supported-operation journal sized from `a` (indices computed from a's counts, so it is valid
// for every golden). Covers every operation family the diff is expected to reproduce.
static std::vector<nlohmann::json> supported_journal(const op::Project& a) {
    using nlohmann::json;
    const int nt = static_cast<int>(a.tracks.size());
    const int np = static_cast<int>(a.patterns.size());
    const int nc = static_cast<int>(a.arrangement.size());
    const int n0 = static_cast<int>(a.patterns[0].notes.size());
    std::vector<json> ops;
    ops.push_back(json{{"op", "set_bpm"}, {"bpm", 97.5}});
    ops.push_back(json{{"op", "set_length_bars"}, {"length_bars", 9.0}});
    ops.push_back(json{{"op", "set_master_gain"}, {"gain_db", -3.0}});
    ops.push_back(json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -2.5}});
    ops.push_back(json{{"op", "set_track_pan"}, {"index", 0}, {"pan", 0.25}});
    ops.push_back(json{{"op", "set_track_mute"}, {"index", 0}, {"mute", true}});
    ops.push_back(json{{"op", "set_track_solo"}, {"index", 0}, {"solo", true}});
    ops.push_back(json{{"op", "rename_track"}, {"index", 0}, {"name", "Renamed"}});
    ops.push_back(json{{"op", "add_track"},
                       {"track", json::parse(R"({"name":"Added","gain_db":0.0,"pan":0.0,)"
                                             R"("voice":{"kind":"sine","freq_hz":330.0}})")}});
    ops.push_back(json{{"op", "add_pattern"},
                       {"pattern", json::parse(R"({"track":)" + std::to_string(nt) +
                                               R"(,"notes":[{"t":0.0,"dur":1.0}]})")}});
    ops.push_back(json{{"op", "remove_note"}, {"pattern", 0}, {"index", n0 - 1}});
    ops.push_back(json{{"op", "add_note"}, {"pattern", 0}, {"note", json::parse(R"({"t":0.5,"dur":0.25,"vel":0.5})")}});
    ops.push_back(json{{"op", "add_clip"},
                       {"clip", json::parse(R"({"pattern":)" + std::to_string(np) +
                                            R"(,"start_bar":2.0,"repeats":2})")}});
    ops.push_back(json{{"op", "move_clip"}, {"index", nc}, {"start_bar", 3.0}});
    ops.push_back(json{{"op", "set_clip_gain"}, {"index", nc}, {"gain_db", -6.0}});
    ops.push_back(json{{"op", "add_attachment"}, {"path", "stems/x.wav"}, {"kind", "stem"}, {"producer", "test"}});
    return ops;
}

static void test_opd_diff() {
    using nlohmann::json;
    std::string err;
    std::vector<json> ops;
    const std::string gdir = std::string(OP_TEST_DATA) + "/fixtures/goldens/";

    // (a) identical projects -> success, empty journal
    op::Project demo;
    CHECK(op::load_project(gdir + "demo.opj", demo, err), "diff.load_demo");
    ops.push_back(json{{"op", "stale"}});  // must be cleared on success
    CHECK(op::diff_projects(demo, demo, ops, err) && ops.empty() && err.empty(), "diff.identical_empty_ok");

    // (b) every golden: hand-made supported journal -> b; diff(a,b) replayed on a gives b
    const char* names[] = {"demo", "mixer", "arranged", "panorama", "pitched", "tempo"};
    for (const char* nm : names) {
        const std::string check_name = std::string("diff.roundtrip_") + nm;
        op::Project a, b;
        std::string e;
        bool ok = op::load_project(gdir + nm + ".opj", a, e);
        b = a;
        ok = ok && op::apply_journal(b, journal_text(supported_journal(a)), e);
        std::vector<json> dops, dops2;
        ok = ok && op::diff_projects(a, b, dops, e) && !dops.empty();
        ok = ok && op::diff_projects(a, b, dops2, e) && journal_text(dops) == journal_text(dops2);  // determinism
        op::Project c = a;
        ok = ok && op::apply_journal(c, journal_text(dops), e) &&
             op::project_to_json(c).dump() == op::project_to_json(b).dump();
        CHECK(ok, check_name.c_str());
    }

    // (c) exact three-op example from the spec
    op::Project ex = demo;
    std::string ee;
    bool built = op::apply_op(ex, json{{"op", "set_bpm"}, {"bpm", 90.0}}, ee) &&
                 op::apply_op(ex, json{{"op", "set_master_gain"}, {"gain_db", -3.0}}, ee) &&
                 op::apply_op(ex, json{{"op", "set_track_gain"}, {"index", 0}, {"gain_db", -2.5}}, ee);
    std::vector<json> eops;
    CHECK(built && op::diff_projects(demo, ex, eops, ee) &&
              journal_text(eops) ==
                  "{\"bpm\":90.0,\"op\":\"set_bpm\"}\n"
                  "{\"gain_db\":-3.0,\"op\":\"set_master_gain\"}\n"
                  "{\"gain_db\":-2.5,\"index\":0,\"op\":\"set_track_gain\"}\n",
          "diff.example_text_exact");

    // (d) order rules: removals descending, remove_clip < remove_pattern < remove_track, then set_bpm
    const op::Project big = diff_base_project();
    op::Project small = big;
    small.tracks.resize(1);
    small.patterns.resize(1);
    small.arrangement.resize(1);
    small.bpm = 133.0;
    std::vector<json> dord;
    bool dok = op::diff_projects(big, small, dord, ee);
    CHECK(dok && journal_text(dord) ==
                     "{\"index\":2,\"op\":\"remove_clip\"}\n"
                     "{\"index\":1,\"op\":\"remove_clip\"}\n"
                     "{\"index\":2,\"op\":\"remove_pattern\"}\n"
                     "{\"index\":1,\"op\":\"remove_pattern\"}\n"
                     "{\"index\":2,\"op\":\"remove_track\"}\n"
                     "{\"index\":1,\"op\":\"remove_track\"}\n"
                     "{\"bpm\":133.0,\"op\":\"set_bpm\"}\n",
          "diff.removal_order_exact");
    op::Project rp = big;
    CHECK(dok && op::apply_journal(rp, journal_text(dord), ee) &&
              op::project_to_json(rp).dump() == op::project_to_json(small).dump(),
          "diff.removal_order_replays");

    // (e) unsupported differences: exact field strings, ops empty, nothing emitted
    const op::Project base = diff_base_project();
    auto expect_unsupported = [&](const op::Project& y, const std::string& field) {
        std::vector<json> o;
        o.push_back(json{{"op", "stale"}});
        std::string e;
        return !op::diff_projects(base, y, o, e) && o.empty() && e == "unsupported difference: " + field;
    };
    op::Project y = base;
    y.seed = base.seed + 1;
    CHECK(expect_unsupported(y, "seed"), "diff.unsupported_seed");
    y = base;
    y.sample_rate = 44100;
    CHECK(expect_unsupported(y, "sample_rate"), "diff.unsupported_sample_rate");
    y = base;
    y.tracks[0].voice.freq_hz = 440.0f;
    CHECK(expect_unsupported(y, "tracks[0].voice"), "diff.unsupported_voice_0");
    y = base;
    y.tempo_map = {op::TempoPoint{0.0, 120.0}};
    CHECK(expect_unsupported(y, "tempo_map"), "diff.unsupported_tempo_map");

    // (f) fixed check order: seed is reported before beats_per_bar
    y = base;
    y.seed = base.seed + 1;
    y.beats_per_bar = 3;
    CHECK(expect_unsupported(y, "seed"), "diff.unsupported_seed_before_beats");
    op::Project pt = base;
    pt.patterns[1].track = 0;
    std::vector<json> upo;
    std::string upe;
    CHECK(!op::diff_projects(base, pt, upo, upe) && upo.empty() &&
              upe == "unsupported difference: patterns[1].track",
          "diff.unsupported_pattern_track_1");

    // (g) WP-0159: clip pattern/repeats are supported (set_clip_pattern / set_clip_repeats).
    // Replay helper: diff(from,to) applied to a copy of `from` must give exactly `to`.
    auto canon = [](const op::Project& p) { return op::project_to_json(p).dump(); };
    auto replays = [&](const op::Project& from, const op::Project& to, std::vector<json>& o) {
        std::string re;
        op::Project c = from;
        return op::diff_projects(from, to, o, re) && op::apply_journal(c, journal_text(o), re) &&
               canon(c) == canon(to);
    };
    const op::Project cbase = diff_base_project();  // clips: (p0,0.0,r1) (p1,1.0,r1) (p2,2.0,r1)

    // (g1) repeats on clip 0 + pattern on clip 1
    op::Project t1 = cbase;
    std::string e1;
    bool t1ok = op::apply_op(t1, json{{"op", "set_clip_repeats"}, {"index", 0}, {"repeats", 3}}, e1) &&
                op::apply_op(t1, json{{"op", "set_clip_pattern"}, {"index", 1}, {"pattern", 2}}, e1);
    std::vector<json> o1;
    CHECK(t1ok && op::diff_projects(cbase, t1, o1, e1) && journal_text(o1) ==
                  "{\"index\":0,\"op\":\"set_clip_repeats\",\"repeats\":3}\n"
                  "{\"index\":1,\"op\":\"set_clip_pattern\",\"pattern\":2}\n",
          "diff.clip_repeats_and_pattern_exact");
    CHECK(t1ok && replays(cbase, t1, o1), "diff.clip_repeats_and_pattern_replays");

    // (g2) all four clip fields changed on one clip: pattern, repeats, move_clip, set_clip_gain
    op::Project t2 = cbase;
    std::string e2;
    bool t2ok = op::apply_op(t2, json{{"op", "set_clip_pattern"}, {"index", 1}, {"pattern", 0}}, e2) &&
                op::apply_op(t2, json{{"op", "set_clip_repeats"}, {"index", 1}, {"repeats", 2}}, e2) &&
                op::apply_op(t2, json{{"op", "move_clip"}, {"index", 1}, {"start_bar", 3.0}}, e2) &&
                op::apply_op(t2, json{{"op", "set_clip_gain"}, {"index", 1}, {"gain_db", -4.0}}, e2);
    std::vector<json> o2;
    CHECK(t2ok && op::diff_projects(cbase, t2, o2, e2) && journal_text(o2) ==
                  "{\"index\":1,\"op\":\"set_clip_pattern\",\"pattern\":0}\n"
                  "{\"index\":1,\"op\":\"set_clip_repeats\",\"repeats\":2}\n"
                  "{\"index\":1,\"op\":\"move_clip\",\"start_bar\":3.0}\n"
                  "{\"gain_db\":-4.0,\"index\":1,\"op\":\"set_clip_gain\"}\n",
          "diff.clip_all_four_fields_exact");
    CHECK(t2ok && replays(cbase, t2, o2), "diff.clip_all_four_fields_replays");

    // (g3) shrinking patterns: clip 2 re-pointed to pattern 0, then pattern 2 removed.
    // Step 1b must emit set_clip_pattern before remove_pattern (else "pattern in use").
    op::Project t3 = cbase;
    std::string e3;
    bool t3ok = op::apply_op(t3, json{{"op", "set_clip_pattern"}, {"index", 2}, {"pattern", 0}}, e3) &&
                op::apply_op(t3, json{{"op", "remove_pattern"}, {"index", 2}}, e3);
    std::vector<json> o3;
    CHECK(t3ok && op::diff_projects(cbase, t3, o3, e3) && journal_text(o3) ==
                  "{\"index\":2,\"op\":\"set_clip_pattern\",\"pattern\":0}\n"
                  "{\"index\":2,\"op\":\"remove_pattern\"}\n",
          "diff.clip_shrink_patterns_exact");
    CHECK(t3ok && replays(cbase, t3, o3), "diff.clip_shrink_patterns_replays");

    // (g4) growing patterns: add_pattern (step 8) precedes set_clip_pattern (step 9)
    op::Project t4 = cbase;
    std::string e4;
    bool t4ok = op::apply_op(t4, json{{"op", "add_pattern"},
                                      {"pattern", json::parse(R"({"track":0,"notes":[{"t":0.0,"dur":0.5}]})")}},
                             e4) &&
                op::apply_op(t4, json{{"op", "set_clip_pattern"}, {"index", 0}, {"pattern", 3}}, e4);
    std::vector<json> o4;
    CHECK(t4ok && op::diff_projects(cbase, t4, o4, e4) && journal_text(o4) ==
                  "{\"op\":\"add_pattern\",\"pattern\":" + op::project_to_json(t4)["patterns"][3].dump() +
                      "}\n{\"index\":0,\"op\":\"set_clip_pattern\",\"pattern\":3}\n",
          "diff.clip_grow_patterns_exact");
    CHECK(t4ok && replays(cbase, t4, o4), "diff.clip_grow_patterns_replays");

    // (g5) duplicate_clip / reorder_clip results: round trip, and the journal never names them
    op::Project t5 = cbase;
    std::string e5;
    bool t5ok = op::apply_op(t5, json{{"op", "duplicate_clip"}, {"index", 0}, {"start_bar", 0.5}}, e5);
    std::vector<json> o5;
    CHECK(t5ok && op::diff_projects(cbase, t5, o5, e5) &&
              journal_text(o5).find("duplicate_clip") == std::string::npos &&
              journal_text(o5).find("reorder_clip") == std::string::npos,
          "diff.duplicate_clip_no_dup_op");
    CHECK(t5ok && replays(cbase, t5, o5), "diff.duplicate_clip_replays");

    op::Project t6 = cbase;
    std::string e6;
    bool t6ok = op::apply_op(t6, json{{"op", "reorder_clip"}, {"index", 0}, {"to", 2}}, e6);
    std::vector<json> o6;
    CHECK(t6ok && op::diff_projects(cbase, t6, o6, e6) &&
              journal_text(o6).find("duplicate_clip") == std::string::npos &&
              journal_text(o6).find("reorder_clip") == std::string::npos,
          "diff.reorder_clip_no_reorder_op");
    CHECK(t6ok && replays(cbase, t6, o6), "diff.reorder_clip_replays");
}

// WP-0150: the polyphase tap table must be bit-identical to the frozen direct reference for every
// rate pair / length / channel count, including the over-budget fallback (44101 → 48000).
static void test_resample_polyphase() {
    // Fixed-seed LCG (no rand): deterministic pseudo-random input.
    struct Lcg {
        unsigned long long s;
        explicit Lcg(unsigned long long seed) : s(seed) {}
        unsigned long long next() {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;
            return s >> 32;
        }
        float unit() {  // 24-bit mantissa → [-1, 1]
            const float u = static_cast<float>(next() >> 8) * (1.0f / 16777215.0f);
            return 2.0f * u - 1.0f;
        }
    };

    unsigned long long scalars_compared = 0;
    auto bit_identical_case = [&scalars_compared](int in_rate, int out_rate, int channels,
                                                  long long frames) -> bool {
        Lcg rng(0x9E3779B97F4A7C15ULL ^ (static_cast<unsigned long long>(in_rate) << 8) ^
                (static_cast<unsigned long long>(out_rate) << 20) ^
                (static_cast<unsigned long long>(channels) << 33) ^
                static_cast<unsigned long long>(frames));
        std::vector<float> x(static_cast<size_t>(frames) * static_cast<size_t>(channels));
        for (float& v : x) v = rng.unit();
        std::vector<float> ref_out, new_out;
        const bool rr = op_test_ref::windowsinc_direct_ref(x, channels, in_rate, out_rate, ref_out);
        const bool rn = op::resample::windowsinc(x, channels, in_rate, out_rate, new_out);
        if (rr != rn) return false;
        if (ref_out.size() != new_out.size()) return false;
        if (!ref_out.empty() &&
            std::memcmp(ref_out.data(), new_out.data(), ref_out.size() * sizeof(float)) != 0)
            return false;
        scalars_compared += static_cast<unsigned long long>(ref_out.size());
        return true;
    };

    const int rates_in[] = {8000, 11025, 22050, 32000, 44100, 88200, 96000, 192000};
    const long long lengths[] = {0, 1, 2, 17, 100, 4801};
    for (int in_rate : rates_in) {
        bool all_ok = true;
        for (int channels : {1, 2})
            for (long long frames : lengths)
                if (!bit_identical_case(in_rate, 48000, channels, frames)) all_ok = false;
        const std::string name =
            "resample.polyphase_bit_identical_" + std::to_string(in_rate) + "_to_48000";
        CHECK(all_ok, name.c_str());
    }
    // 44101 → 48000: gcd = 1 → phases × W exceeds the table budget → over-budget fallback path.
    {
        bool all_ok = true;
        for (int channels : {1, 2})
            for (long long frames : lengths)
                if (!bit_identical_case(44101, 48000, channels, frames)) all_ok = false;
        CHECK(all_ok, "resample.polyphase_bit_identical_44101_to_48000_over_budget");
    }
    // Downsampling pair: 48000 → 44100.
    {
        bool all_ok = true;
        for (int channels : {1, 2})
            for (long long frames : lengths)
                if (!bit_identical_case(48000, 44100, channels, frames)) all_ok = false;
        CHECK(all_ok, "resample.polyphase_bit_identical_48000_to_44100");
    }

    // The output vector is fully overwritten (assign) even when pre-filled with garbage of a
    // different size.
    {
        Lcg rng(0xDEADBEEFCAFEBABEULL);
        std::vector<float> x(static_cast<size_t>(4801) * 2);
        for (float& v : x) v = rng.unit();
        std::vector<float> ref_out;
        op_test_ref::windowsinc_direct_ref(x, 2, 44100, 48000, ref_out);
        std::vector<float> out(99997, 1.2345e30f);  // pre-filled garbage, wrong size
        const bool ok = op::resample::windowsinc(x, 2, 44100, 48000, out);
        bool clean = ok && out.size() == ref_out.size();
        if (clean && !ref_out.empty())
            clean = std::memcmp(ref_out.data(), out.data(), ref_out.size() * sizeof(float)) == 0;
        CHECK(clean, "resample.polyphase_prefilled_garbage_overwritten");
    }

    std::printf("INFO resample polyphase: %llu float scalars bit-compared vs reference\n",
                scalars_compared);
}

// WP-0154 step sequencer (docs/spec/operations.md §Step sequencer): set_step / clear_steps / step_grid.
static void test_step_sequencer() {
    using nlohmann::json;
    auto base = []() {
        op::Project p;
        p.bpm = 120.0; p.length_bars = 4.0; p.sample_rate = 48000; p.seed = 7;
        op::Track t0; t0.name = "Kick"; t0.voice.kind = op::VoiceKind::Sine; t0.voice.freq_hz = 220.0f;
        p.tracks = {t0};
        op::Pattern pt; pt.track = 0;
        p.patterns = {pt};
        return p;
    };
    auto set_step = [](long long s, bool on) {
        return json{{"op", "set_step"}, {"pattern", 0}, {"step", s}, {"on", on}};
    };
    auto dump = [](const op::Project& p) { return op::project_to_json(p).dump(); };
    auto note = [](double t, double dur) { op::Note n; n.t_beats = t; n.dur_beats = dur; return n; };

    // (1) default step note: no pitch key, no vel key, t = s*0.25, dur = 0.25
    op::Project p1 = base();
    std::string e1;
    CHECK(op::apply_op(p1, set_step(0, true), e1) && p1.patterns[0].notes.size() == 1 &&
              p1.patterns[0].notes[0].t_beats == 0.0 && p1.patterns[0].notes[0].dur_beats == 0.25 &&
              p1.patterns[0].notes[0].midi_note == -1 && p1.patterns[0].notes[0].velocity == 1.0,
          "step.set_on_default_note");

    // (2) with velocity + pitch: appended note carries both, t/dur still on the grid
    op::Project p2 = base();
    std::string e2;
    CHECK(op::apply_op(p2, json{{"op", "set_step"}, {"pattern", 0}, {"step", 4}, {"on", true},
                                {"velocity", 0.5}, {"pitch", 36}}, e2) &&
              p2.patterns[0].notes.size() == 1 && p2.patterns[0].notes[0].t_beats == 1.0 &&
              p2.patterns[0].notes[0].dur_beats == 0.25 && p2.patterns[0].notes[0].midi_note == 36 &&
              p2.patterns[0].notes[0].velocity == 0.5,
          "step.set_on_velocity_pitch");

    // (3) set_step twice -> identical canonical bytes after the second application
    op::Project p3 = base();
    std::string e3;
    bool twice = op::apply_op(p3, set_step(8, true), e3);
    const std::string once_bytes = dump(p3);
    twice = twice && op::apply_op(p3, set_step(8, true), e3);
    CHECK(twice && dump(p3) == once_bytes, "step.set_twice_identical_bytes");

    // (4) on:false removes every note on the step, keeps off-grid t=0.3 and another step's note
    op::Project p4 = base();
    p4.patterns[0].notes = {note(1.0, 0.25), note(0.3, 0.25), note(1.0, 0.5), note(2.0, 0.25)};
    std::string e4;
    CHECK(op::apply_op(p4, set_step(4, false), e4) && p4.patterns[0].notes.size() == 2 &&
              p4.patterns[0].notes[0].t_beats == 0.3 && p4.patterns[0].notes[1].t_beats == 2.0,
          "step.off_removes_all_on_step_keeps_offgrid");

    // (5) on:true on an existing long note: only vel/pitch may change, dur stays 2.0
    op::Project p5 = base();
    p5.patterns[0].notes = {note(0.0, 2.0)};
    std::string e5;
    CHECK(op::apply_op(p5, json{{"op", "set_step"}, {"pattern", 0}, {"step", 0}, {"on", true},
                                {"velocity", 0.25}}, e5) &&
              p5.patterns[0].notes.size() == 1 && p5.patterns[0].notes[0].dur_beats == 2.0 &&
              p5.patterns[0].notes[0].velocity == 0.25,
          "step.on_existing_keeps_dur");

    // (6) clear_steps keeps off-grid notes, removes on-grid ones (including step 30 = beat 7.5)
    op::Project p6 = base();
    p6.patterns[0].notes = {note(0.0, 0.25), note(0.3, 0.25), note(1.0, 0.25), note(7.5, 0.25)};
    std::string e6;
    CHECK(op::apply_op(p6, json{{"op", "clear_steps"}, {"pattern", 0}}, e6) &&
              p6.patterns[0].notes.size() == 1 && p6.patterns[0].notes[0].t_beats == 0.3,
          "step.clear_keeps_offgrid");

    // (7) rejection classes leave the project byte-for-byte unchanged
    op::Project fx = base();
    fx.patterns[0].notes = {note(0.0, 0.25)};
    const std::string before = dump(fx);
    auto rejects = [&](const json& o, const char* want, const char* name) {
        op::Project q = fx;
        std::string e;
        const bool ok = op::apply_op(q, o, e);
        CHECK(!ok && e == want && dump(q) == before, name);
    };
    rejects(set_step(4096, true), "set_step: 'step' out of range", "step.reject_step_4096");
    rejects(set_step(-1, true), "set_step: 'step' out of range", "step.reject_step_minus1");
    rejects(json{{"op", "set_step"}, {"pattern", 0}, {"step", 1.0}, {"on", true}},
            "set_step: 'step' must be an integer", "step.reject_step_float");
    rejects(json{{"op", "set_step"}, {"pattern", 0}, {"step", 0}, {"on", true}, {"pitch", 128}},
            "set_step: 'pitch' out of range", "step.reject_pitch_128");
    rejects(json{{"op", "set_step"}, {"pattern", 0}, {"step", 4}, {"on", true}, {"velocity", 0}},
            "set_step: note.vel must be in (0, 1]", "step.reject_vel_zero");
    rejects(json{{"op", "set_step"}, {"pattern", 0}, {"step", 0}, {"on", true}, {"velocity", 1.5}},
            "set_step: note.vel must be in (0, 1]", "step.reject_vel_above_one");
    rejects(json{{"op", "set_step"}, {"pattern", 0}, {"step", 0}, {"on", true}, {"foo", 1}},
            "set_step: unknown argument 'foo'", "step.reject_unknown_argument");
    rejects(json{{"op", "set_step"}, {"pattern", 0}, {"step", 0}, {"on", false}, {"velocity", 0.5}},
            "set_step: 'velocity' not allowed when 'on' is false", "step.reject_velocity_on_false");

    // (8) step_grid: 16 steps, 8 steps, 0 steps; off-grid and beyond-range notes are not shown
    op::Project g = base();
    g.patterns[0].notes = {note(0.0, 0.25), note(1.0, 0.25), note(2.0, 0.25), note(3.0, 0.25),
                           note(0.3, 0.25), note(5.0, 0.25)};
    CHECK(op::step_grid(g.patterns[0], 16) == "x...x...x...x...", "step.grid_16");
    CHECK(op::step_grid(g.patterns[0], 8) == "x...x...", "step.grid_8");
    CHECK(op::step_grid(g.patterns[0], 0).empty(), "step.grid_0_empty");

    // (9) History undo/redo restores the bytes exactly
    op::History h;
    op::Project hp = base();
    std::string eh;
    const std::string b0 = dump(hp);
    bool hok = h.apply(hp, set_step(0, true), eh);
    const std::string b1 = dump(hp);
    hok = hok && h.apply(hp, set_step(4, true), eh);
    const std::string b2 = dump(hp);
    hok = hok && h.undo(hp) && dump(hp) == b1;
    hok = hok && h.undo(hp) && dump(hp) == b0;
    hok = hok && h.redo(hp) && dump(hp) == b1;
    hok = hok && h.redo(hp) && dump(hp) == b2;
    CHECK(hok, "step.history_undo_redo_bytes");

    // (10) determinism: four set_step ops render byte-identically to hand-written notes at beats 0..3
    op::Project sq = base();
    sq.length_bars = 1.0;
    std::string es;
    bool sok = true;
    for (long long s : {0LL, 4LL, 8LL, 12LL}) sok = sok && op::apply_op(sq, set_step(s, true), es);
    op::Project hand = base();
    hand.length_bars = 1.0;
    hand.patterns[0].notes = {note(0.0, 0.25), note(1.0, 0.25), note(2.0, 0.25), note(3.0, 0.25)};
    op::wav::StereoBuffer ra, rb;
    std::string er;
    sok = sok && op::render_project(sq, "", ra, er) && op::render_project(hand, "", rb, er);
    CHECK(sok && !ra.interleaved.empty() && ra.interleaved.size() == rb.interleaved.size() &&
              std::memcmp(ra.interleaved.data(), rb.interleaved.data(),
                          ra.interleaved.size() * sizeof(float)) == 0,
          "step.render_byte_identical_to_handwritten");
}

// --- WP-0155: transport loop region (docs/spec/project-model.md §Loop region, operations.md) ------

// Minimal valid project text; `extra` is spliced in before the closing brace (e.g. a "loop" member).
static std::string loop_project_text(const std::string& extra, double length_bars = 4.0) {
    return std::string(R"({"format_version":0,"bpm":120,"length_bars":)") + std::to_string(length_bars) +
           R"(,"tracks":[{"name":"T","voice":{"kind":"sine","freq_hz":220.0}}],)" +
           R"("patterns":[{"track":0,"notes":[{"t":0.0,"dur":1.0}]}])" + extra + "}";
}

// Parse text → Project; returns the load error (empty on success) through `err_out`.
static bool parse_text(const std::string& text, op::Project& p, std::string& err_out) {
    write_text(tmp_file("op_loop_in.opj"), text);
    return op::load_project(tmp_file("op_loop_in.opj"), p, err_out);
}

static void test_loop_region() {
    using nlohmann::json;
    std::string err;

    // Parse/save round trip with a fractional loop: values kept, second save byte-identical.
    op::Project p;
    const bool parsed = parse_text(loop_project_text(R"(,"loop":{"start_bar":0.5,"end_bar":2.5})"), p, err);
    CHECK(parsed && p.loop && p.loop->start_bar == 0.5 && p.loop->end_bar == 2.5, "loop.parse_fractional");
    const std::string b1 = op::project_bytes(p);
    op::Project back;
    CHECK(parse_text(b1, back, err) && back.loop && back.loop->start_bar == 0.5 && back.loop->end_bar == 2.5 &&
              op::project_bytes(back) == b1,
          "loop.roundtrip_byte_identical");
    CHECK(b1.find("\"loop\": {\n    \"end_bar\": 2.5,\n    \"start_bar\": 0.5\n  }") != std::string::npos,
          "loop.canonical_key_order");

    // No loop → no key in the saved bytes, and identical to the same project parsed without a loop.
    op::Project nl;
    CHECK(parse_text(loop_project_text(""), nl, err) && !nl.loop && op::project_bytes(nl).find("loop") == std::string::npos,
          "loop.absent_not_saved");

    // Each validation error, verbatim, first failure wins (spec order 1..7).
    auto expect_err = [&](const std::string& text, const std::string& want, const char* name) {
        op::Project q;
        std::string e;
        const bool ok = parse_text(text, q, e);
        CHECK(!ok && e == want, name);
    };
    expect_err(loop_project_text(R"(,"loop":[1,2])"), "loop must be an object", "loop.err_not_object");
    expect_err(loop_project_text(R"(,"loop":{"start_bar":"1","end_bar":2})"),
               "loop.start_bar and loop.end_bar must be numbers", "loop.err_not_number");
    expect_err(loop_project_text(R"(,"loop":{"end_bar":2})"),
               "loop.start_bar and loop.end_bar must be numbers", "loop.err_missing_start");
    expect_err(loop_project_text(R"(,"loop":{"start_bar":-0.5,"end_bar":2})"),
               "loop.start_bar must be a finite number >= 0", "loop.err_start_negative");
    // JSON text cannot spell inf (nlohmann rejects 1e999 at parse), so feed an infinite double to the
    // engine-internal parse directly: that is the only way check 4 can be reached.
    {
        json jinf = json::parse(loop_project_text(R"(,"loop":{"start_bar":1,"end_bar":2})"));
        jinf["loop"]["end_bar"] = std::numeric_limits<double>::infinity();
        op::Project q;
        std::string e;
        CHECK(!op::project_from_json(jinf, q, e) && e == "loop.end_bar must be a finite number",
              "loop.err_end_not_finite");
    }
    expect_err(loop_project_text(R"(,"loop":{"start_bar":2,"end_bar":2})"),
               "loop.end_bar must be greater than loop.start_bar", "loop.err_end_not_after_start");
    expect_err(loop_project_text(R"(,"loop":{"start_bar":1,"end_bar":5})"),
               "loop.end_bar must be <= length_bars", "loop.err_end_past_length");
    expect_err(loop_project_text(R"(,"loop":{"start_bar":1,"end_bar":2,"extra":1})"),
               "loop has unknown key 'extra'", "loop.err_unknown_key");

    // set_loop / clear_loop through apply_op; undo restores the prior bytes exactly.
    op::Project q;
    CHECK(parse_text(loop_project_text(""), q, err), "loop.ops_base_parse");
    const std::string base_bytes = op::project_bytes(q);
    op::History h;
    CHECK(h.apply(q, json{{"op", "set_loop"}, {"start_bar", 1.0}, {"end_bar", 3.0}}, err) && q.loop &&
              q.loop->start_bar == 1.0 && q.loop->end_bar == 3.0, "loop.op_set");
    const std::string with_loop = op::project_bytes(q);
    CHECK(h.apply(q, json{{"op", "clear_loop"}}, err) && !q.loop, "loop.op_clear");
    CHECK(op::project_bytes(q) == base_bytes, "loop.op_clear_bytes_match_no_loop");
    CHECK(h.undo(q) && op::project_bytes(q) == with_loop, "loop.undo_clear_restores_loop");
    CHECK(h.undo(q) && op::project_bytes(q) == base_bytes, "loop.undo_set_restores_bytes");
    CHECK(h.redo(q) && op::project_bytes(q) == with_loop, "loop.redo_set");

    // Argument rules and range rules come from the parse (exact op error strings).
    op::Project r;
    CHECK(parse_text(loop_project_text(""), r, err), "loop.ops_rules_parse");
    CHECK(!op::apply_op(r, json{{"op", "set_loop"}, {"start_bar", 1.0}}, err) &&
              err == "set_loop: missing argument 'end_bar'", "loop.op_set_missing_end");
    CHECK(!op::apply_op(r, json{{"op", "set_loop"}, {"start_bar", "x"}, {"end_bar", 2.0}}, err) &&
              err == "set_loop: 'start_bar' must be a number", "loop.op_set_not_number");
    CHECK(!op::apply_op(r, json{{"op", "set_loop"}, {"start_bar", 3.0}, {"end_bar", 2.0}}, err) &&
              err == "set_loop: loop.end_bar must be greater than loop.start_bar", "loop.op_set_reversed");
    CHECK(!op::apply_op(r, json{{"op", "clear_loop"}, {"x", 1}}, err) &&
              err == "clear_loop: unknown argument 'x'", "loop.op_clear_unknown_arg");
    CHECK(op::apply_op(r, json{{"op", "clear_loop"}}, err) && !r.loop, "loop.op_clear_noop_ok");

    // set_length_bars below an existing loop end is rejected and leaves the project untouched.
    op::Project s;
    CHECK(parse_text(loop_project_text(R"(,"loop":{"start_bar":1,"end_bar":3})"), s, err), "loop.len_parse");
    CHECK(!op::apply_op(s, json{{"op", "set_length_bars"}, {"length_bars", 2.0}}, err) &&
              err == "set_length_bars: loop.end_bar must be <= length_bars" && s.length_bars == 4.0 && s.loop,
          "loop.set_length_below_end_rejected");

    // opd diff with a loop on one side: journal reproduces the target exactly.
    op::Project none = s, with = s, moved = s, cleared = s;
    none.loop.reset();
    with.loop = op::LoopRegion{0.5, 2.5};
    moved.loop = op::LoopRegion{1.0, 2.0};
    cleared.loop.reset();
    auto round_trip = [&](const op::Project& a, const op::Project& b, const char* name) {
        std::vector<json> ops;
        std::string e;
        if (!op::diff_projects(a, b, ops, e)) { CHECK(false, name); return; }
        op::Project work = a;
        bool ok = true;
        for (const json& o : ops) ok = ok && op::apply_op(work, o, e);
        CHECK(ok && op::project_bytes(work) == op::project_bytes(b), name);
    };
    round_trip(none, with, "loop.diff_add_loop");
    round_trip(with, none, "loop.diff_remove_loop");
    round_trip(with, moved, "loop.diff_change_loop");
    round_trip(moved, cleared, "loop.diff_change_then_clear");
    std::vector<json> same_ops;
    CHECK(op::diff_projects(with, with, same_ops, err) && same_ops.empty(), "loop.diff_same_loop_no_ops");
}

// ---------------------------------------------------------------- sampler trim (WP-0156)
// Parse one-track project whose voice is `voice` (pan -1 so output left = source sample exactly).
static bool trim_parse(const nlohmann::json& voice, op::Project& p, std::string& err) {
    nlohmann::json j;
    j["format_version"] = 0; j["bpm"] = 120.0; j["sample_rate"] = 48000;
    j["length_bars"] = 1.0; j["beats_per_bar"] = 4; j["seed"] = 7;
    nlohmann::json tk = {{"name", "S"}, {"gain_db", 0.0}, {"pan", -1.0}, {"voice", voice}};
    j["tracks"] = nlohmann::json::array();
    j["tracks"].push_back(tk);
    nlohmann::json note = {{"t", 0.0}, {"dur", 4.0}};
    nlohmann::json pat = {{"track", 0}};
    pat["notes"] = nlohmann::json::array();
    pat["notes"].push_back(note);
    j["patterns"] = nlohmann::json::array();
    j["patterns"].push_back(pat);
    return op::project_from_json(j, p, err);
}

// Left-channel samples of a render of p (base_dir /tmp for the fixture WAVs written below).
static bool trim_render_left(const op::Project& p, std::vector<float>& left, std::string& err) {
    op::wav::StereoBuffer b;
    if (!op::render_project(p, tmp_dir(), b, err)) return false;
    left.resize(b.frames());
    for (size_t i = 0; i < left.size(); ++i) left[i] = b.interleaved[2 * i];
    return true;
}

static void test_sampler_trim() {
    using nlohmann::json;
    std::string err;

    // Fixture asset: 1000 mono frames, frame i = i/32768 (PCM16-exact).
    std::vector<float> ramp(1000);
    for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / 32768.0f;
    write_mono_pcm16(tmp_file("op_trim_ramp.wav"), 48000, ramp);

    // (1)-(2) parse: fields stored; untouched voice keeps defaults.
    op::Project pa;
    const json va = {{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.0125}, {"end_s", 0.0375}};
    CHECK(trim_parse(va, pa, err) && pa.tracks[0].voice.start_s == 0.0125 &&
              pa.tracks[0].voice.has_end_s && pa.tracks[0].voice.end_s == 0.0375,
          "trim.parse_fields");
    op::Project pd;
    const json vd = {{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}};
    CHECK(trim_parse(vd, pd, err) && pd.tracks[0].voice.start_s == 0.0 && !pd.tracks[0].voice.has_end_s,
          "trim.parse_defaults_untrimmed");

    // (3)-(9) parse errors, exact strings, first failure wins.
    const std::string e_start = "voice.start_s must be a finite number >= 0";
    const std::string e_end = "voice.end_s must be a finite number > 0";
    const std::string e_order = "voice.end_s must be greater than voice.start_s";
    op::Project scratch;
    CHECK(!trim_parse({{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", -0.5}}, scratch, err) &&
              err == e_start, "trim.e_start_negative");
    CHECK(!trim_parse({{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", "0.1"}}, scratch, err) &&
              err == e_start, "trim.e_start_not_number");
    CHECK(!trim_parse({{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"end_s", 0.0}}, scratch, err) &&
              err == e_end, "trim.e_end_zero");
    CHECK(!trim_parse({{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"end_s", nullptr}}, scratch, err) &&
              err == e_end, "trim.e_end_null");
    CHECK(!trim_parse({{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.5}, {"end_s", 0.25}},
                      scratch, err) && err == e_order, "trim.e_end_not_after_start");
    CHECK(!trim_parse({{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.5}, {"end_s", 0.5}},
                      scratch, err) && err == e_order, "trim.e_end_equals_start");
    CHECK(!trim_parse({{"kind", "metronome"}, {"start_s", 0.1}}, scratch, err) &&
              err == "voice.start_s/end_s are only valid for sampler voices", "trim.e_non_sampler");

    // (10)-(11) canonical save: default keys omitted; trimmed keys emitted; round-trip stable.
    const std::string bytes_untrimmed = op::project_bytes(pd);
    CHECK(bytes_untrimmed.find("start_s") == std::string::npos &&
              bytes_untrimmed.find("end_s") == std::string::npos, "trim.canonical_omits_defaults");
    const std::string bytes_trim = op::project_bytes(pa);
    CHECK(bytes_trim.find("\"start_s\": 0.0125") != std::string::npos &&
              bytes_trim.find("\"end_s\": 0.0375") != std::string::npos, "trim.canonical_emits_trim");
    op::Project back;
    CHECK(op::project_from_json(json::parse(bytes_trim), back, err) && op::project_bytes(back) == bytes_trim,
          "trim.canonical_roundtrip_stable");

    // (12)-(13) exact slice: slice [96, 480) of the ramp; gate-free note of 4 beats.
    op::Project ps;
    const json vs = {{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.002}, {"end_s", 0.01}};
    CHECK(trim_parse(vs, ps, err), "trim.parse_slice_ok");
    std::vector<float> slice, full;
    CHECK(trim_render_left(ps, slice, err), "trim.render_slice_ok");
    CHECK(trim_parse({{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}}, scratch, err) &&
              trim_render_left(scratch, full, err), "trim.render_reference_ok");
    bool exact = true, silent_after = true;
    for (size_t n = 0; n < 384; ++n) exact = exact && slice[n] == full[96 + n];
    for (size_t n = 384; n < slice.size(); ++n) silent_after = silent_after && slice[n] == 0.0f;
    CHECK(exact, "trim.slice_samples_exact");
    CHECK(silent_after && slice[383] != 0.0f, "trim.silence_after_slice");

    // (14) note shorter than the slice gates it (0.001 beat = 24 frames at 120 bpm / 48 kHz).
    op::Project pg = ps;
    pg.patterns[0].notes[0].dur_beats = 0.001;
    std::vector<float> gated;
    CHECK(trim_render_left(pg, gated, err) && gated[23] == full[96 + 23] && gated[24] == 0.0f &&
              gated[100] == 0.0f, "trim.note_gates_slice");

    // (15) end_s beyond the asset clamps to the asset length: slice [96, 1000).
    op::Project pc;
    const json vc = {{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.002}, {"end_s", 10.0}};
    std::vector<float> clamped;
    CHECK(trim_parse(vc, pc, err) && trim_render_left(pc, clamped, err) && clamped[903] == full[96 + 903] &&
              clamped[904] == 0.0f, "trim.end_clamps_to_asset");

    // (16)-(17) start at/after asset end: render error; start inside asset with no end: short slice.
    op::Project pe;
    const json ve = {{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.05}};
    CHECK(trim_parse(ve, pe, err), "trim.parse_start_beyond_ok");
    op::wav::StereoBuffer be;
    CHECK(!op::render_project(pe, tmp_dir(), be, err) && err == "sampler: trim slice is empty",
          "trim.e_render_slice_empty");
    op::Project pz;
    const json vz = {{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.02}};
    std::vector<float> tail;
    CHECK(trim_parse(vz, pz, err) && trim_render_left(pz, tail, err) && tail[39] == full[960 + 39] &&
              tail[40] == 0.0f, "trim.start_only_to_asset_end");

    // (18) 44.1 kHz asset: slice is i1 - i0 frames at 48 kHz (960 .. 2880 -> 1920 frames).
    std::vector<float> dc(4410, 0.5f);
    write_mono_pcm16(tmp_file("op_trim_dc44k.wav"), 44100, dc);
    op::Project pr;
    const json vr = {{"kind", "sampler"}, {"path", "op_trim_dc44k.wav"}, {"start_s", 0.02}, {"end_s", 0.06}};
    std::vector<float> rs;
    CHECK(trim_parse(vr, pr, err) && trim_render_left(pr, rs, err), "trim.resampled_asset_renders");
    size_t sounding = 0;
    for (float x : rs) sounding += x != 0.0f ? 1u : 0u;
    CHECK(sounding == 1920 && rs[1919] != 0.0f && rs[1920] == 0.0f, "trim.resampled_slice_length_48k");

    // (19) explicit default trim renders bit-identical to the untrimmed path.
    op::Project pdef;
    const json vdef = {{"kind", "sampler"}, {"path", "op_trim_ramp.wav"}, {"start_s", 0.0}};
    std::vector<float> defr;
    CHECK(trim_parse(vdef, pdef, err) && trim_render_left(pdef, defr, err) && defr.size() == full.size() &&
              std::memcmp(defr.data(), full.data(), full.size() * sizeof(float)) == 0,
          "trim.default_render_bitexact");

    // (20)-(27) set_sampler_trim op: sets, clears with null, rejects with exact strings.
    op::Project po;
    op::Track tk; tk.name = "S"; tk.pan = -1.0f;
    tk.voice.kind = op::VoiceKind::Sampler; tk.voice.path = "op_trim_ramp.wav";
    po.bpm = 120.0; po.length_bars = 1.0; po.beats_per_bar = 4; po.sample_rate = 48000;
    po.tracks.push_back(tk);
    op::Track mk; mk.name = "M"; mk.voice.kind = op::VoiceKind::Metronome;
    po.tracks.push_back(mk);
    CHECK(op::apply_op(po, json{{"op", "set_sampler_trim"}, {"index", 0}, {"start_s", 0.01}, {"end_s", 0.02}}, err) &&
              po.tracks[0].voice.start_s == 0.01 && po.tracks[0].voice.has_end_s &&
              po.tracks[0].voice.end_s == 0.02, "ops.set_sampler_trim_sets");
    CHECK(op::apply_op(po, json{{"op", "set_sampler_trim"}, {"index", 0}, {"start_s", 0}, {"end_s", nullptr}}, err) &&
              po.tracks[0].voice.start_s == 0.0 && !po.tracks[0].voice.has_end_s,
          "ops.set_sampler_trim_null_clears");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_trim"}, {"index", 0}, {"start_s", 0.03}, {"end_s", 0.02}}, err) &&
              err == "set_sampler_trim: voice.end_s must be greater than voice.start_s",
          "ops.set_sampler_trim_reject_order");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_trim"}, {"index", 0}, {"start_s", -1}, {"end_s", 1.0}}, err) &&
              err == "set_sampler_trim: voice.start_s must be a finite number >= 0",
          "ops.set_sampler_trim_reject_start_range");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_trim"}, {"index", 1}, {"start_s", 0.0}, {"end_s", nullptr}}, err) &&
              err == "set_sampler_trim: track is not a sampler voice",
          "ops.set_sampler_trim_reject_non_sampler");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_trim"}, {"index", 0}, {"start_s", "x"}, {"end_s", nullptr}}, err) &&
              err == "set_sampler_trim: 'start_s' must be a number", "ops.set_sampler_trim_reject_start_type");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_trim"}, {"index", 0}, {"start_s", 0.0}}, err) &&
              err == "set_sampler_trim: missing argument 'end_s'", "ops.set_sampler_trim_reject_end_missing");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_trim"}, {"index", 0}, {"start_s", 0.0}, {"end_s", "x"}}, err) &&
              err == "set_sampler_trim: 'end_s' must be a number or null", "ops.set_sampler_trim_reject_end_type");

    std::printf("INFO sampler trim: slice frames [96,480) checked against untrimmed reference\n");
}

// --- Synth voice v1 (WP-0161): parse of waveform/adsr/cutoff_hz/resonance, defaults, exact
// validation messages, canonical save, release tail, and the set_synth_params op.
static void test_synth_v1() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    std::string err;

    // Scratch dir under the per-process tmp_dir() (WP-0160 helpers).
    const fs::path dir = fs::path(tmp_file("op_synth_v1"));
    std::error_code fec;
    fs::remove_all(dir, fec);
    fs::create_directories(dir, fec);

    auto parse_voice = [](const json& voice, op::Project& p, std::string& err) {
        json j;
        j["format_version"] = 0; j["bpm"] = 120.0; j["sample_rate"] = 48000; j["seed"] = 161;
        j["length_bars"] = 1.0; j["beats_per_bar"] = 4;
        json tj; tj["name"] = "T"; tj["voice"] = voice;
        j["tracks"] = json::array({tj});
        return op::project_from_json(j, p, err);
    };

    // (1) Parse of every new key: fields land on the model.
    op::Project pv;
    json v1;
    v1["kind"] = "synth";
    v1["waveform"] = "square";
    v1["adsr"] = {{"attack_s", 0.02}, {"decay_s", 0.3}, {"sustain", 0.6}, {"release_s", 0.4}};
    v1["cutoff_hz"] = 1234.0;
    v1["resonance"] = 5.5;
    CHECK(parse_voice(v1, pv, err) &&
              pv.tracks[0].voice.waveform == op::Waveform::Square && pv.tracks[0].voice.has_adsr &&
              pv.tracks[0].voice.attack_s == 0.02 && pv.tracks[0].voice.decay_s == 0.3 &&
              pv.tracks[0].voice.sustain == 0.6 && pv.tracks[0].voice.release_s == 0.4 &&
              pv.tracks[0].voice.cutoff_hz == 1234.0 && pv.tracks[0].voice.resonance == 5.5,
          "synth_v1.parse_all_keys");

    // (2) Waveform strings map to the enum; absent = Legacy (which is NOT Sine).
    op::Project psine, psaw, pleg;
    CHECK(parse_voice({{"kind", "synth"}, {"waveform", "sine"}}, psine, err) &&
              psine.tracks[0].voice.waveform == op::Waveform::Sine, "synth_v1.parse_waveform_sine");
    CHECK(parse_voice({{"kind", "synth"}, {"waveform", "saw"}}, psaw, err) &&
              psaw.tracks[0].voice.waveform == op::Waveform::Saw, "synth_v1.parse_waveform_saw");
    CHECK(parse_voice({{"kind", "synth"}}, pleg, err) &&
              pleg.tracks[0].voice.waveform == op::Waveform::Legacy && !pleg.tracks[0].voice.has_adsr &&
              pleg.tracks[0].voice.cutoff_hz == 0.0 && pleg.tracks[0].voice.resonance == 0.7071,
          "synth_v1.parse_defaults_absent");

    // (3) adsr present but empty → inner defaults 0.005 / 0 / 1 / 0.03.
    op::Project pad;
    CHECK(parse_voice({{"kind", "synth"}, {"adsr", json::object()}}, pad, err) &&
              pad.tracks[0].voice.has_adsr && pad.tracks[0].voice.attack_s == 0.005 &&
              pad.tracks[0].voice.decay_s == 0.0 && pad.tracks[0].voice.sustain == 1.0 &&
              pad.tracks[0].voice.release_s == 0.03, "synth_v1.adsr_empty_defaults");

    // (4) Validation messages, character-exact, first failure wins.
    op::Project scratch;
    CHECK(!parse_voice({{"kind", "synth"}, {"waveform", "triangle"}}, scratch, err) &&
              err == "voice.waveform must be sine|saw|square", "synth_v1.e_waveform_value");
    CHECK(!parse_voice({{"kind", "synth"}, {"waveform", 5}}, scratch, err) &&
              err == "voice.waveform must be sine|saw|square", "synth_v1.e_waveform_not_string");
    CHECK(!parse_voice({{"kind", "synth"}, {"adsr", 5}}, scratch, err) &&
              err == "voice.adsr must be an object", "synth_v1.e_adsr_not_object");
    CHECK(!parse_voice({{"kind", "synth"}, {"adsr", {{"wobble", 1}}}}, scratch, err) &&
              err == "voice.adsr has unknown key 'wobble'", "synth_v1.e_adsr_unknown_key");
    CHECK(!parse_voice({{"kind", "synth"}, {"adsr", {{"sustain", 2}}}}, scratch, err) &&
              err == "voice.adsr.sustain must be a number in [0, 1]", "synth_v1.e_adsr_sustain_range");
    CHECK(!parse_voice({{"kind", "synth"}, {"adsr", {{"release_s", -1}}}}, scratch, err) &&
              err == "voice.adsr.release_s must be a number in [0, 10]", "synth_v1.e_adsr_release_range");
    CHECK(!parse_voice({{"kind", "synth"}, {"cutoff_hz", 10}}, scratch, err) &&
              err == "voice.cutoff_hz must be a number in [20, 20000]", "synth_v1.e_cutoff_range");
    CHECK(!parse_voice({{"kind", "synth"}, {"resonance", 40}}, scratch, err) &&
              err == "voice.resonance must be a number in [0.5, 20]", "synth_v1.e_resonance_range");
    // Non-synth voice: the first present key in spec order (waveform, adsr, cutoff_hz, resonance).
    CHECK(!parse_voice({{"kind", "metronome"}, {"resonance", 3.0}, {"cutoff_hz", 100.0}}, scratch, err) &&
              err == "voice.cutoff_hz is only valid for synth voices", "synth_v1.e_non_synth_cutoff_first");
    CHECK(!parse_voice({{"kind", "metronome"}, {"resonance", 3.0}, {"adsr", json::object()}}, scratch, err) &&
              err == "voice.adsr is only valid for synth voices", "synth_v1.e_non_synth_adsr_first");

    // (5) Canonical save: adsr writes all four inner keys; set resonance written; default
    // resonance/waveform/cutoff_hz omitted; legacy voice emits none of the new keys.
    const std::string bytes_v1 = op::project_bytes(pv);
    CHECK(bytes_v1.find("\"attack_s\"") != std::string::npos &&
              bytes_v1.find("\"decay_s\"") != std::string::npos &&
              bytes_v1.find("\"sustain\"") != std::string::npos &&
              bytes_v1.find("\"release_s\"") != std::string::npos &&
              bytes_v1.find("\"resonance\": 5.5") != std::string::npos, "synth_v1.canonical_adsr_all_four");
    const std::string bytes_pad = op::project_bytes(pad);  // adsr present, everything else default
    CHECK(bytes_pad.find("\"adsr\"") != std::string::npos &&
              bytes_pad.find("\"resonance\"") == std::string::npos &&
              bytes_pad.find("\"waveform\"") == std::string::npos &&
              bytes_pad.find("\"cutoff_hz\"") == std::string::npos, "synth_v1.canonical_omits_defaults");
    const std::string bytes_leg = op::project_bytes(pleg);
    CHECK(bytes_leg.find("waveform") == std::string::npos && bytes_leg.find("adsr") == std::string::npos &&
              bytes_leg.find("cutoff_hz") == std::string::npos && bytes_leg.find("resonance") == std::string::npos,
          "synth_v1.legacy_voice_bytes_unchanged");
    op::Project back;
    CHECK(op::project_from_json(json::parse(bytes_v1), back, err) && op::project_bytes(back) == bytes_v1,
          "synth_v1.canonical_roundtrip_stable");

    // (6) File save/load through a temp dir named with the pid.
    const fs::path rt = dir / "roundtrip.opj";
    op::Project back2;
    CHECK(op::save_project(rt.string(), pv, err) && op::load_project(rt.string(), back2, err) &&
              back2.tracks[0].voice.waveform == op::Waveform::Square &&
              back2.tracks[0].voice.has_adsr && back2.tracks[0].voice.cutoff_hz == 1234.0,
          "synth_v1.file_save_load_roundtrip");

    // (7) Release tail: sine + adsr{A=0,D=0,S=0.5,R=0.1}, note 0.25 beats @120bpm → len=6000,
    // tail=ceil(0.1*48000)=4800: audio after note-off, silent from len+tail on; repeat render bit-exact.
    op::Project pt;
    pt.bpm = 120.0; pt.length_bars = 1.0; pt.beats_per_bar = 4; pt.sample_rate = 48000;
    op::Track vt; vt.name = "V";
    vt.voice.kind = op::VoiceKind::Synth;
    vt.voice.waveform = op::Waveform::Sine;
    vt.voice.has_adsr = true;
    vt.voice.attack_s = 0.0; vt.voice.decay_s = 0.0; vt.voice.sustain = 0.5; vt.voice.release_s = 0.1;
    pt.tracks.push_back(vt);
    op::Pattern pat; pat.track = 0;
    op::Note nt; nt.t_beats = 0.0; nt.dur_beats = 0.25; nt.midi_note = 69;
    pat.notes.push_back(nt);
    pt.patterns.push_back(pat);

    const long long len = static_cast<long long>(std::llround(0.25 * 24000.0));
    const long long tail = static_cast<long long>(std::ceil(vt.voice.release_s * 48000.0));
    op::wav::StereoBuffer bt;
    CHECK(op::render_project(pt, "", bt, err), "synth_v1.tail_render_ok");
    bool tail_nonzero = false;
    for (long long n = len; n < len + tail; ++n)
        if (bt.interleaved[static_cast<size_t>(n) * 2] != 0.0f) { tail_nonzero = true; break; }
    CHECK(tail_nonzero, "synth_v1.release_tail_present_after_note_off");
    bool silent_after = true;
    for (size_t n = static_cast<size_t>(len + tail); n * 2 < bt.interleaved.size(); ++n)
        if (bt.interleaved[n * 2] != 0.0f || bt.interleaved[n * 2 + 1] != 0.0f) { silent_after = false; break; }
    CHECK(silent_after, "synth_v1.silent_after_len_plus_tail");
    op::wav::StereoBuffer bt2;
    CHECK(op::render_project(pt, "", bt2, err) && bt.interleaved == bt2.interleaved,
          "synth_v1.render_bitexact_repeat");

    // (8) e_off rule: decay completes before note-off with sustain 0 → note-off level 0 → the
    // release tail is silent (hard silence from frame len on).
    op::Project pz = pt;
    pz.tracks[0].voice.decay_s = 0.05; pz.tracks[0].voice.sustain = 0.0; pz.tracks[0].voice.release_s = 0.2;
    op::wav::StereoBuffer bz;
    bool body_nonzero = false;
    CHECK(op::render_project(pz, "", bz, err), "synth_v1.eoff_render_ok");
    for (long long n = 0; n < len; ++n)
        if (bz.interleaved[static_cast<size_t>(n) * 2] != 0.0f) { body_nonzero = true; break; }
    bool silent_eoff = true;
    for (size_t n = static_cast<size_t>(len); n * 2 < bz.interleaved.size(); ++n)
        if (bz.interleaved[n * 2] != 0.0f) { silent_eoff = false; break; }
    CHECK(body_nonzero && silent_eoff, "synth_v1.e_off_zero_tail_silent");

    // (9) set_synth_params: sets (inner adsr defaults for keys not given), null resets all four
    // keys byte-identically, rejects non-synth track / no params / unknown key / bad ranges.
    op::Project po;
    op::Track tk; tk.name = "S"; tk.voice.kind = op::VoiceKind::Synth;
    op::Track mk; mk.name = "M"; mk.voice.kind = op::VoiceKind::Metronome;
    po.bpm = 120.0; po.length_bars = 1.0; po.beats_per_bar = 4; po.sample_rate = 48000;
    po.tracks.push_back(tk); po.tracks.push_back(mk);
    const std::string bytes_orig = op::project_bytes(po);

    CHECK(op::apply_op(po, json{{"op", "set_synth_params"}, {"index", 0}, {"waveform", "saw"},
                                {"adsr", {{"attack_s", 0.02}}}, {"cutoff_hz", 800.0}}, err) &&
              po.tracks[0].voice.waveform == op::Waveform::Saw && po.tracks[0].voice.has_adsr &&
              po.tracks[0].voice.attack_s == 0.02 && po.tracks[0].voice.decay_s == 0.0 &&
              po.tracks[0].voice.sustain == 1.0 && po.tracks[0].voice.release_s == 0.03 &&
              po.tracks[0].voice.cutoff_hz == 800.0, "synth_v1.op_set_params_inner_defaults");

    CHECK(op::apply_op(po, json{{"op", "set_synth_params"}, {"index", 0}, {"waveform", nullptr},
                                {"adsr", nullptr}, {"cutoff_hz", nullptr}, {"resonance", nullptr}}, err) &&
              !po.tracks[0].voice.has_adsr && po.tracks[0].voice.waveform == op::Waveform::Legacy &&
              po.tracks[0].voice.cutoff_hz == 0.0 && op::project_bytes(po) == bytes_orig,
          "synth_v1.op_null_reset_byte_identical");

    CHECK(rejected_unchanged(po, json{{"op", "set_synth_params"}, {"index", 1}, {"waveform", "sine"}}, err) &&
              err == "set_synth_params: track is not a synth voice", "synth_v1.op_reject_not_synth");
    CHECK(rejected_unchanged(po, json{{"op", "set_synth_params"}, {"index", 0}}, err) &&
              err == "set_synth_params: no synth parameters given", "synth_v1.op_reject_no_params");
    CHECK(rejected_unchanged(po, json{{"op", "set_synth_params"}, {"index", 0}, {"bogus", 1}}, err) &&
              err == "set_synth_params: unknown argument 'bogus'", "synth_v1.op_reject_unknown_key");
    CHECK(rejected_unchanged(po, json{{"op", "set_synth_params"}, {"index", 0}, {"waveform", "triangle"}}, err) &&
              err == "set_synth_params: voice.waveform must be sine|saw|square", "synth_v1.op_reject_waveform_value");
    CHECK(rejected_unchanged(po, json{{"op", "set_synth_params"}, {"index", 0}, {"cutoff_hz", 5.0}}, err) &&
              err == "set_synth_params: voice.cutoff_hz must be a number in [20, 20000]", "synth_v1.op_reject_cutoff_range");

    fs::remove_all(dir, fec);
    std::printf("INFO synth v1: %lld-frame release tail verified after note-off\n", tail);
}

// ---------------------------------------------------------------- pattern swing (WP-0166)

static void test_pattern_swing() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    std::string err;

    // Unique temp dir (pid-suffixed): never fixed /tmp/op_* paths.
    const fs::path dir = fs::path(tmp_file("op_swing"));
    fs::create_directories(dir);

    // Known-content "kick": 4800 mono frames of constant 0.5 (PCM16-exact, never zero), so every
    // note onset is a rising edge out of silence on the hard-left channel.
    std::vector<float> kick(4800, 0.5f);
    write_mono_pcm16((dir / "op_swing_kick.wav").string(), 48000, kick);

    // One-bar... five-bar sampler project: notes at `ts` (dur 0.04 beats → 960 frames @120bpm,
    // gaps keep onsets separable), optional pattern swing value, optional tempo map.
    auto proj_json = [](const std::vector<double>& ts, bool has_swing, const json& swing_val,
                        const json& tempo_map) {  // pass json(nullptr) for "no tempo map"
        json j;
        j["format_version"] = 0; j["bpm"] = 120.0; j["sample_rate"] = 48000;
        j["length_bars"] = 5.0; j["beats_per_bar"] = 4; j["seed"] = 7;
        json voice = {{"kind", "sampler"}, {"path", "op_swing_kick.wav"}};
        json tk = {{"name", "S"}, {"gain_db", 0.0}, {"pan", -1.0}, {"voice", voice}};
        j["tracks"] = json::array();
        j["tracks"].push_back(tk);
        json pat = {{"track", 0}};
        if (has_swing) pat["swing"] = swing_val;
        pat["notes"] = json::array();
        for (double t : ts) pat["notes"].push_back({{"t", t}, {"dur", 0.04}});
        j["patterns"] = json::array();
        j["patterns"].push_back(pat);
        if (!tempo_map.is_null()) j["tempo_map"] = tempo_map;
        return j;
    };

    // Left-channel onset samples (rising edges 0 → non-zero) of a render.
    auto render_onsets = [&](const op::Project& p, std::vector<long long>& v) -> bool {
        op::wav::StereoBuffer b;
        if (!op::render_project(p, dir.string(), b, err)) return false;
        v.clear();
        float prev = 0.0f;
        for (size_t i = 0; i < b.frames(); ++i) {
            const float x = b.interleaved[2 * i];
            if (x != 0.0f && prev == 0.0f) v.push_back(static_cast<long long>(i));
            prev = x;
        }
        return true;
    };

    // (1) parse stores the value; canonical save keeps it at 0.5.
    op::Project p_half;
    CHECK(op::project_from_json(proj_json({0.25}, true, json(0.5), json(nullptr)), p_half, err) &&
              p_half.patterns[0].swing == 0.5 &&
              op::project_bytes(p_half).find("\"swing\": 0.5") != std::string::npos,
          "swing.parse_and_save_keeps_value");

    // (2)-(5) validation: exact message for each bad kind (negative, > 0.75, string, null).
    const std::string e_swing = "pattern.swing must be a number in [0, 0.75]";
    op::Project scratch;
    CHECK(!op::project_from_json(proj_json({0.25}, true, json(-0.1), json(nullptr)), scratch, err) &&
              err == e_swing, "swing.e_negative");
    CHECK(!op::project_from_json(proj_json({0.25}, true, json(0.76), json(nullptr)), scratch, err) &&
              err == e_swing, "swing.e_above_range");
    CHECK(!op::project_from_json(proj_json({0.25}, true, json("0.5"), json(nullptr)), scratch, err) &&
              err == e_swing, "swing.e_string");
    CHECK(!op::project_from_json(proj_json({0.25}, true, json(nullptr), json(nullptr)), scratch, err) &&
              err == e_swing, "swing.e_null");

    // (6) -0.0 accepted and treated as 0 (canonical omits the key). Match the quoted key: the
    // fixture path "op_swing_kick.wav" also contains the word "swing".
    op::Project p_mzero;
    CHECK(op::project_from_json(proj_json({0.25}, true, json(-0.0), json(nullptr)), p_mzero, err) &&
              p_mzero.patterns[0].swing == 0.0 &&
              op::project_bytes(p_mzero).find("\"swing\"") == std::string::npos, "swing.minus_zero_treated_as_zero");

    // (7) canonical save omits swing when exactly 0.
    const std::vector<double> sixteenths = {0.0, 0.25, 0.5, 0.75};
    op::Project p_noswing;
    CHECK(op::project_from_json(proj_json(sixteenths, false, json(), json(nullptr)), p_noswing, err) &&
              op::project_bytes(p_noswing).find("\"swing\"") == std::string::npos, "swing.canonical_omits_zero");

    // (8) explicit swing 0.0 renders bit-identically to no-swing (default path untouched).
    {
        op::Project p_zero;
        bool ok = op::project_from_json(proj_json(sixteenths, true, json(0.0), json(nullptr)), p_zero, err);
        std::vector<long long> v1, v2;
        ok = ok && render_onsets(p_zero, v1) && render_onsets(p_noswing, v2) && v1 == v2 && !v1.empty();
        op::wav::StereoBuffer b1, b2;
        ok = ok && op::render_project(p_zero, dir.string(), b1, err) &&
             op::render_project(p_noswing, dir.string(), b2, err) &&
             b1.interleaved.size() == b2.interleaved.size() &&
             std::memcmp(b1.interleaved.data(), b2.interleaved.data(),
                         b1.interleaved.size() * sizeof(float)) == 0;
        CHECK(ok, "swing.render_zero_equals_no_swing_bitexact");
    }

    // (9) 16th pattern @120bpm/48kHz, swing 0.5 → delay 0.0625 beats = 1500 samples: odd steps
    // at llround((t + 0.0625)*24000) (step 1: 7500), even steps unmoved.
    {
        std::vector<long long> v;
        CHECK(op::project_from_json(proj_json(sixteenths, true, json(0.5), json(nullptr)), p_half, err) &&
                  render_onsets(p_half, v) && v == std::vector<long long>({0, 7500, 12000, 19500}),
              "swing.onsets_odd_steps_shifted_even_unmoved");
    }

    // (10) off-grid note (t = 0.3) does not move under swing.
    {
        op::Project p_off;
        std::vector<long long> v;
        CHECK(op::project_from_json(proj_json({0.3}, true, json(0.5), json(nullptr)), p_off, err) &&
                  render_onsets(p_off, v) && v == std::vector<long long>({7200}),  // llround(0.3*24000)
              "swing.off_grid_note_unmoved");
    }

    // (11) under a tempo map ({bar 0: 120, bar 1.95: 60}): onset = TempoMap::at(shifted t_abs),
    // segment chosen by the shifted position (t=7.75+0.0625 crosses the change at beat 7.8).
    {
        json tm = json::array();
        tm.push_back(json{{"bar", 0.0}, {"bpm", 120.0}});
        tm.push_back(json{{"bar", 1.95}, {"bpm", 60.0}});
        op::Project p_tm;
        std::vector<long long> v;
        CHECK(op::project_from_json(proj_json({0.25, 7.75, 8.0}, true, json(0.5), tm), p_tm, err) &&
                  render_onsets(p_tm, v) &&
                  v == std::vector<long long>({7500,          // seg0: llround(0.3125*24000)
                                               187800,        // seg1: 187200 + llround(0.0125*48000)
                                               196800}),      // even step: 187200 + llround(0.2*48000)
              "swing.tempo_map_onsets_shifted_segment_by_position");
    }

    // (12)-(15) set_pattern_swing op: sets, clears to byte-identical defaults, rejects bad values
    // with the spec'd strings (project unchanged on rejection).
    {
        op::Project p = p_noswing;
        const std::string bytes_never_swung = op::project_bytes(p);
        CHECK(op::apply_op(p, json{{"op", "set_pattern_swing"}, {"pattern", 0}, {"swing", 0.5}}, err) &&
                  p.patterns[0].swing == 0.5 &&
                  op::project_bytes(p).find("\"swing\": 0.5") != std::string::npos,
              "ops.set_pattern_swing_sets");
        CHECK(op::apply_op(p, json{{"op", "set_pattern_swing"}, {"pattern", 0}, {"swing", 0.0}}, err) &&
                  op::project_bytes(p) == bytes_never_swung, "ops.set_pattern_swing_zero_clears_bytes");
        const std::string before = op::project_bytes(p);
        CHECK(!op::apply_op(p, json{{"op", "set_pattern_swing"}, {"pattern", 0}, {"swing", 0.76}}, err) &&
                  err == "set_pattern_swing: " + e_swing && op::project_bytes(p) == before,
              "ops.set_pattern_swing_reject_range");
        CHECK(!op::apply_op(p, json{{"op", "set_pattern_swing"}, {"pattern", 0}, {"swing", "x"}}, err) &&
                  err == "set_pattern_swing: 'swing' must be a number" && op::project_bytes(p) == before,
              "ops.set_pattern_swing_reject_type");
    }

    // (16) undo restores the exact bytes; redo re-applies.
    {
        op::Project p = p_noswing;
        const std::string before = op::project_bytes(p);
        op::History h;
        bool ok = h.apply(p, json{{"op", "set_pattern_swing"}, {"pattern", 0}, {"swing", 0.5}}, err) &&
                  op::project_bytes(p).find("\"swing\": 0.5") != std::string::npos;
        ok = ok && h.undo(p) && op::project_bytes(p) == before;
        ok = ok && h.redo(p) && p.patterns[0].swing == 0.5;
        CHECK(ok, "ops.set_pattern_swing_undo_restores_bytes");
    }

    // (17)-(18) opd diff: projects differing only in pattern swing emit exactly the op; applying
    // the journal to a reproduces b byte-for-byte.
    {
        op::Project a = p_noswing, b = p_noswing;
        CHECK(op::apply_op(b, json{{"op", "set_pattern_swing"}, {"pattern", 0}, {"swing", 0.5}}, err),
              "diff.swing_build_b_ok");
        std::vector<json> ops;
        bool ok = op::diff_projects(a, b, ops, err) &&
                  journal_text(ops) == "{\"op\":\"set_pattern_swing\",\"pattern\":0,\"swing\":0.5}\n";
        ok = ok && op::apply_journal(a, journal_text(ops), err) &&
             op::project_to_json(a).dump() == op::project_to_json(b).dump();
        CHECK(ok, "diff.swing_emits_and_replays_op");
    }

    fs::remove_all(dir);
}

// ---------------------------------------------------------------- sampler repitch (WP-0167)
// Ratio table and read-position rule transcribed straight from docs/spec/project-model.md
// §Sampler repitch so the test checks the engine against the spec text, not just against itself.
static double rp_ratio(int shift) {
    static const double kT[12] = {1.0,               1.0594630943592953, 1.122462048309373,
                                  1.189207115002721, 1.2599210498948732, 1.3348398541700344,
                                  1.4142135623730951, 1.4983070768766815, 1.5874010519681996,
                                  1.681792830507429, 1.7817974362806785, 1.887748625363387};
    int q = shift / 12;          // truncating division → fix up to floor for negative shifts
    int r = shift - 12 * q;
    if (r < 0) { r += 12; --q; }
    double ratio = kT[r];
    for (int i = 0; i < q; ++i) ratio *= 2.0;
    for (int i = 0; i > q; --i) ratio /= 2.0;
    return ratio;
}

// The spec's output frame n of the repitched slice x[from .. from+len); false when the source has run
// out (the interpolation needs x[k+1]). Usage: rp_sample(x, 0, x.size(), rp_ratio(7), 3, y).
static bool rp_sample(const std::vector<float>& x, size_t from, size_t len, double ratio, long long n, float& y) {
    const double pos = static_cast<double>(n) * ratio;
    const long long k = static_cast<long long>(pos);  // pos >= 0 → truncation IS the floor
    if (k + 1 >= static_cast<long long>(len)) return false;
    const double frac = pos - static_cast<double>(k);
    const double a = x[from + static_cast<size_t>(k)], b = x[from + static_cast<size_t>(k) + 1];
    y = static_cast<float>(a + frac * (b - a));
    return true;
}

// One-track sampler project; pan -1 → left gain exactly 1.0, pan +1 → right gain exactly 1.0, so a
// channel can be compared to the raw source without any gain arithmetic. Notes carry optional "note" pitch.
static bool rp_parse(const nlohmann::json& voice, const nlohmann::json& notes, op::Project& p, std::string& err,
                     double pan = -1.0) {
    nlohmann::json j;
    j["format_version"] = 0; j["bpm"] = 120.0; j["sample_rate"] = 48000;
    j["length_bars"] = 1.0; j["beats_per_bar"] = 4; j["seed"] = 7;
    nlohmann::json tk = {{"name", "S"}, {"gain_db", 0.0}, {"pan", pan}, {"voice", voice}};
    j["tracks"] = nlohmann::json::array();
    j["tracks"].push_back(tk);
    nlohmann::json pat = {{"track", 0}};
    pat["notes"] = notes;
    j["patterns"] = nlohmann::json::array();
    j["patterns"].push_back(pat);
    return op::project_from_json(j, p, err);
}

// Full interleaved buffer of a render with base_dir `dir` (assets live in the WP-0167 temp dir).
static bool rp_render(const op::Project& p, const std::string& dir, op::wav::StereoBuffer& b, std::string& err) {
    return op::render_project(p, dir, b, err);
}

static void test_sampler_repitch() {
    using nlohmann::json;
    std::string err;

    // Unique temp dir for generated assets (parallel CI runs must not collide); removed at the end.
    const std::filesystem::path tmp = tmp_file("op_repitch");
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);
    const std::string dir = tmp.string();

    // Assets: mono ramp frame i = i/32768 (PCM16-exact); stereo ramp L[i]=i/32768, R[i]=(999-i)/32768.
    const size_t N = 1000;
    std::vector<float> ramp(N);
    for (size_t i = 0; i < N; ++i) ramp[i] = static_cast<float>(i) / 32768.0f;
    write_mono_pcm16(dir + "/op_rp_ramp.wav", 48000, ramp);
    {
        op::wav::StereoBuffer sb;
        sb.sample_rate = 48000;
        sb.interleaved.resize(N * 2);
        for (size_t i = 0; i < N; ++i) {
            sb.interleaved[2 * i] = ramp[i];
            sb.interleaved[2 * i + 1] = static_cast<float>(999 - i) / 32768.0f;
        }
        CHECK(op::wav::write_wav_stereo(dir + "/op_rp_stereo.wav", sb, op::wav::SampleFormat::Pcm16, err),
              "repitch.stereo_asset_written");
    }

    // (1)-(2) parse: root stored; absent stays -1.
    op::Project pa;
    const json va = {{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"root_note", 60}};
    CHECK(rp_parse(va, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 72}}}), pa, err) &&
              pa.tracks[0].voice.root_note == 60,
          "repitch.parse_stored");
    op::Project pd;
    const json vd = {{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}};
    CHECK(rp_parse(vd, json::array({{{"t", 0.0}, {"dur", 4.0}}}), pd, err) && pd.tracks[0].voice.root_note == -1,
          "repitch.parse_default_absent");

    // (3)-(8) parse errors: exact strings; trim keys still checked first (WP-0156 order).
    op::Project scratch;
    CHECK(!rp_parse({{"kind", "metronome"}, {"root_note", 60}}, json::array(), scratch, err) &&
              err == "voice.root_note is only valid for sampler voices", "repitch.e_non_sampler");
    const std::string e_int = "voice.root_note must be an integer 0..127";
    CHECK(!rp_parse({{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"root_note", "60"}}, json::array(), scratch,
                    err) && err == e_int, "repitch.e_string");
    CHECK(!rp_parse({{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"root_note", 60.0}}, json::array(), scratch,
                    err) && err == e_int, "repitch.e_float");
    CHECK(!rp_parse({{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"root_note", nullptr}}, json::array(), scratch,
                    err) && err == e_int, "repitch.e_null");
    CHECK(!rp_parse({{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"root_note", -1}}, json::array(), scratch,
                    err) && err == e_int, "repitch.e_below_range");
    CHECK(!rp_parse({{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"root_note", 128}}, json::array(), scratch,
                    err) && err == e_int, "repitch.e_above_range");
    CHECK(!rp_parse({{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"start_s", -1.0}, {"root_note", 60}},
                    json::array(), scratch, err) &&
              err == "voice.start_s must be a finite number >= 0", "repitch.e_trim_first");

    // (9)-(11) canonical save: omitted when absent; emitted after end_s when present; round-trip stable.
    const std::string bytes_absent = op::project_bytes(pd);
    CHECK(bytes_absent.find("root_note") == std::string::npos, "repitch.canonical_omits_missing");
    op::Project pwe;
    const json vwe = {{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"end_s", 0.5}, {"root_note", 60}};
    CHECK(rp_parse(vwe, json::array({{{"t", 0.0}, {"dur", 4.0}}}), pwe, err), "repitch.parse_end_plus_root_ok");
    const std::string bytes_both = op::project_bytes(pwe);
    const size_t pos_end = bytes_both.find("\"end_s\"");
    const size_t pos_root = bytes_both.find("\"root_note\": 60");
    CHECK(pos_end != std::string::npos && pos_root != std::string::npos && pos_end < pos_root,
          "repitch.canonical_emits_after_end_s");
    op::Project back;
    CHECK(op::project_from_json(json::parse(bytes_both), back, err) && op::project_bytes(back) == bytes_both &&
              back.tracks[0].voice.root_note == 60, "repitch.canonical_roundtrip_stable");

    // (12)-(13) +12 semitones: ratio exactly 2.0 → out[n] == x[2n]; source runs out at n = 500.
    op::Project p12;
    const json v12 = {{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"root_note", 60}};
    CHECK(rp_parse(v12, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 72}}}), p12, err), "repitch.p12_parse");
    op::wav::StereoBuffer b12;
    CHECK(rp_render(p12, dir, b12, err), "repitch.p12_render");
    bool doublings_exact = true;
    for (long long n = 0; n < 500; ++n) {
        const float want = ramp[static_cast<size_t>(2 * n)];
        doublings_exact = doublings_exact && b12.interleaved[2 * n] == want && b12.interleaved[2 * n + 1] == 0.0f;
    }
    CHECK(doublings_exact, "repitch.p12_out_equals_x2n");
    bool silent_after_12 = true;
    for (size_t n = 500; n < b12.frames(); ++n) silent_after_12 = silent_after_12 && b12.interleaved[2 * n] == 0.0f;
    CHECK(silent_after_12 && b12.interleaved[998] != 0.0f, "repitch.p12_source_runout_silence");

    // (14)-(15) -12 semitones: ratio exactly 0.5 → even frames copy x[m], odd frames exact midpoints.
    op::Project pm12;
    CHECK(rp_parse(v12, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 48}}}), pm12, err), "repitch.pm12_parse");
    op::wav::StereoBuffer bm12;
    CHECK(rp_render(pm12, dir, bm12, err), "repitch.pm12_render");
    bool midpoints_exact = true;
    for (long long n = 0; n < 1998; ++n) {
        const size_t m = static_cast<size_t>(n / 2);
        const float want = (n % 2 == 0) ? ramp[m]
                                        : static_cast<float>((static_cast<double>(ramp[m]) + static_cast<double>(ramp[m + 1])) * 0.5);
        midpoints_exact = midpoints_exact && bm12.interleaved[2 * n] == want;
    }
    CHECK(midpoints_exact, "repitch.pm12_interpolated_midpoints");
    bool silent_after_m12 = true;
    for (size_t n = 1998; n < bm12.frames(); ++n) silent_after_m12 = silent_after_m12 && bm12.interleaved[2 * n] == 0.0f;
    CHECK(silent_after_m12 && bm12.interleaved[2 * 1997] != 0.0f, "repitch.pm12_source_runout_silence");

    // (16)-(17) +7: every sounding frame matches the in-test spec recomputation within 1 LSB (1/32768).
    op::Project pp7;
    CHECK(rp_parse(v12, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 67}}}), pp7, err), "repitch.p7_parse");
    op::wav::StereoBuffer bp7;
    CHECK(rp_render(pp7, dir, bp7, err), "repitch.p7_render");
    const double r7 = rp_ratio(7);
    bool p7_matches = true;
    long long p7_frames = 0;
    for (long long n = 0; n < static_cast<long long>(bp7.frames()); ++n) {
        float want;
        if (!rp_sample(ramp, 0, N, r7, n, want)) break;
        p7_matches = p7_matches && std::fabs(bp7.interleaved[2 * n] - want) <= 1.0f / 32768.0f;
        ++p7_frames;
    }
    CHECK(p7_matches && p7_frames > 600, "repitch.p7_matches_spec_formula_1lsb");
    bool silent_after_p7 = true;
    for (long long n = p7_frames; n < static_cast<long long>(bp7.frames()); ++n)
        silent_after_p7 = silent_after_p7 && bp7.interleaved[2 * n] == 0.0f;
    CHECK(silent_after_p7, "repitch.p7_note_ends_at_source_runout");

    // (18) short note gates the repitched sample (0.001 beat = 24 frames at 120 bpm / 48 kHz).
    op::Project pg7 = pp7;
    pg7.patterns[0].notes[0].dur_beats = 0.001;
    op::wav::StereoBuffer bg7;
    float g_want = 0.0f;
    CHECK(rp_render(pg7, dir, bg7, err) && rp_sample(ramp, 0, N, r7, 23, g_want) &&
              bg7.interleaved[2 * 23] == g_want && bg7.interleaved[2 * 24] == 0.0f, "repitch.p7_short_note_gates");

    // (19)-(20) shift 0 paths render byte-identical to the same project without a root: P == root, and
    // note pitch absent (uses the root). Left channel compared frame by frame against the unrooted render.
    op::Project pzero;
    CHECK(rp_parse(v12, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 60}}}), pzero, err), "repitch.pzero_parse");
    op::wav::StereoBuffer bzero, bnoroot;
    CHECK(rp_render(pzero, dir, bzero, err) && rp_render(pd, dir, bnoroot, err) &&
              std::memcmp(bzero.interleaved.data(), bnoroot.interleaved.data(),
                          bnoroot.interleaved.size() * sizeof(float)) == 0, "repitch.p_eq_root_bitexact");
    op::Project pnopitch;
    CHECK(rp_parse(v12, json::array({{{"t", 0.0}, {"dur", 4.0}}}), pnopitch, err), "repitch.pnopitch_parse");
    op::wav::StereoBuffer bnoop;
    CHECK(rp_render(pnopitch, dir, bnoop, err) &&
              std::memcmp(bnoop.interleaved.data(), bnoroot.interleaved.data(),
                          bnoroot.interleaved.size() * sizeof(float)) == 0, "repitch.absent_pitch_bitexact");

    // (21)-(22) stereo asset: both channels interpolate independently (+5 shift). Rendered twice —
    // hard-left for the L comparison, hard-right for the R comparison — so gains are exactly 1.0.
    op::Project pst;
    const json vst = {{"kind", "sampler"}, {"path", "op_rp_stereo.wav"}, {"root_note", 60}};
    CHECK(rp_parse(vst, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 65}}}), pst, err), "repitch.stereo_parse");
    op::Project pst_r;
    CHECK(rp_parse(vst, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 65}}}), pst_r, err, 1.0),
          "repitch.stereo_parse_right");
    std::vector<float> sl(N), sr(N);
    for (size_t i = 0; i < N; ++i) { sl[i] = ramp[i]; sr[i] = static_cast<float>(999 - i) / 32768.0f; }
    const double r5 = rp_ratio(5);
    bool stereo_left_ok = true, stereo_right_ok = true;
    for (int side = 0; side < 2; ++side) {
        op::wav::StereoBuffer bst;
        if (!rp_render(side == 0 ? pst : pst_r, dir, bst, err)) { stereo_left_ok = stereo_right_ok = false; break; }
        for (long long n = 0; n < static_cast<long long>(bst.frames()); ++n) {
            float wl, wr;
            if (!rp_sample(sl, 0, N, r5, n, wl)) break;
            const bool ok_r = rp_sample(sr, 0, N, r5, n, wr);
            if (!ok_r) { stereo_left_ok = stereo_right_ok = false; break; }
            const float got_l = bst.interleaved[2 * n], got_r = bst.interleaved[2 * n + 1];
            if (side == 0) stereo_left_ok = stereo_left_ok && std::fabs(got_l - wl) <= 1.0f / 32768.0f;
            else stereo_right_ok = stereo_right_ok && std::fabs(got_r - wr) <= 1.0f / 32768.0f;
        }
    }
    CHECK(stereo_left_ok, "repitch.stereo_channel_left_repitched");
    CHECK(stereo_right_ok, "repitch.stereo_channel_right_repitched");

    // (22)-(23) slice stage runs BEFORE repitch: slice [96, 480) of the ramp, +12 → out[n] == x[96+2n].
    op::Project ptrim;
    const json vtrim = {{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"start_s", 0.002}, {"end_s", 0.01},
                        {"root_note", 60}};
    CHECK(rp_parse(vtrim, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 72}}}), ptrim, err),
          "repitch.trimmed_slice_parse");
    op::wav::StereoBuffer btrim;
    bool trim_matches = true;
    if (rp_render(ptrim, dir, btrim, err)) {
        for (long long n = 0; n < 192; ++n)  // slice L=384: last read pair x[478],x[479] at n=191
            trim_matches = trim_matches && btrim.interleaved[2 * n] == ramp[static_cast<size_t>(96 + 2 * n)];
        for (long long n = 192; n < static_cast<long long>(btrim.frames()); ++n)
            trim_matches = trim_matches && btrim.interleaved[2 * n] == 0.0f;
    } else {
        trim_matches = false;
    }
    CHECK(trim_matches, "repitch.trimmed_slice_is_repitched");

    // (24) a slice of L = 1 yields silence, not an error.
    op::Project pone;
    const json vone = {{"kind", "sampler"}, {"path", "op_rp_ramp.wav"}, {"start_s", 0.002}, {"end_s", 0.00203},
                       {"root_note", 60}};
    const bool one_parsed = rp_parse(vone, json::array({{{"t", 0.0}, {"dur", 4.0}, {"note", 72}}}), pone, err);
    op::wav::StereoBuffer bone;
    const bool one_rendered = one_parsed && rp_render(pone, dir, bone, err);
    bool one_quiet = true;
    for (float x : bone.interleaved) if (x != 0.0f) one_quiet = false;
    CHECK(one_parsed && one_rendered && one_quiet, "repitch.slice_len_1_is_silence");

    // (25)-(31) set_sampler_root op: sets, clears with null, rejects with exact strings + unchanged file.
    op::Project po;
    op::Track tk; tk.name = "S"; tk.pan = -1.0f;
    tk.voice.kind = op::VoiceKind::Sampler; tk.voice.path = "op_rp_ramp.wav";
    po.bpm = 120.0; po.length_bars = 1.0; po.beats_per_bar = 4; po.sample_rate = 48000;
    po.tracks.push_back(tk);
    op::Track mk; mk.name = "M"; mk.voice.kind = op::VoiceKind::Metronome;
    po.tracks.push_back(mk);
    CHECK(op::apply_op(po, json{{"op", "set_sampler_root"}, {"index", 0}, {"root_note", 60}}, err) &&
              po.tracks[0].voice.root_note == 60, "ops.set_sampler_root_sets");
    CHECK(op::project_bytes(po).find("\"root_note\": 60") != std::string::npos, "ops.set_sampler_root_saved");
    CHECK(op::apply_op(po, json{{"op", "set_sampler_root"}, {"index", 0}, {"root_note", nullptr}}, err) &&
              po.tracks[0].voice.root_note == -1 && op::project_bytes(po).find("root_note") == std::string::npos,
          "ops.set_sampler_root_null_clears");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_root"}, {"index", 0}, {"root_note", 128}}, err) &&
              err == "set_sampler_root: voice.root_note must be an integer 0..127", "ops.set_sampler_root_reject_128");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_root"}, {"index", 0}, {"root_note", -1}}, err) &&
              err == "set_sampler_root: voice.root_note must be an integer 0..127", "ops.set_sampler_root_reject_neg");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_root"}, {"index", 0}, {"root_note", 60.5}}, err) &&
              err == "set_sampler_root: 'root_note' must be an integer or null", "ops.set_sampler_root_reject_float");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_root"}, {"index", 1}, {"root_note", 60}}, err) &&
              err == "set_sampler_root: track is not a sampler voice", "ops.set_sampler_root_reject_non_sampler");
    CHECK(rejected_unchanged(po, json{{"op", "set_sampler_root"}, {"index", 0}}, err) &&
              err == "set_sampler_root: missing argument 'root_note'", "ops.set_sampler_root_reject_missing");

    std::filesystem::remove_all(tmp);
    std::printf("INFO sampler repitch: +12 doubling, -12 midpoints, +7 spec recomputation checked\n");
}

// --- AIFF / AIFF-C reader (WP-0158): test-side byte builders (big-endian, explicit assembly).
namespace aiff_t {
using Bytes = std::vector<uint8_t>;

void app(Bytes& v, const char* s4) { v.insert(v.end(), s4, s4 + 4); }
void app_u16(Bytes& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x));
}
void app_u32(Bytes& v, uint32_t x) {
    for (int s = 24; s >= 0; s -= 8) v.push_back(static_cast<uint8_t>(x >> s));
}
void app_bytes(Bytes& v, const Bytes& b) { v.insert(v.end(), b.begin(), b.end()); }

// 80-bit extended encoding of an integer rate (normalised: explicit integer bit set).
Bytes ext80(uint64_t rate) {
    int p = 0;
    while ((rate >> p) > 1) ++p;                      // highest set bit position
    const uint64_t m = rate << (63 - p);
    const uint32_t exp = static_cast<uint32_t>(p) + 16383u;
    Bytes out;
    app_u16(out, exp);
    app_u32(out, static_cast<uint32_t>(m >> 32));
    app_u32(out, static_cast<uint32_t>(m));
    return out;
}

Bytes chunk(const char* id, const Bytes& body) {
    Bytes out;
    app(out, id);
    app_u32(out, static_cast<uint32_t>(body.size()));
    app_bytes(out, body);
    if (body.size() & 1u) out.push_back(0);           // pad byte for odd sizes
    return out;
}

// COMM body. aifc_comp == nullptr writes a plain AIFF COMM (18 bytes).
Bytes comm_body(uint32_t channels, uint32_t frames, uint32_t sample_size, const Bytes& rate10,
                const char* aifc_comp = nullptr) {
    Bytes b;
    app_u16(b, channels);
    app_u32(b, frames);
    app_u16(b, sample_size);
    app_bytes(b, rate10);
    if (aifc_comp) {
        app(b, aifc_comp);
        b.push_back(3);                                // pascal string "abc" (ignored by the reader)
        b.push_back('a'); b.push_back('b'); b.push_back('c');
    }
    return b;
}

// SSND body: offset, blockSize (0), `offset` zero bytes, then the sample data.
Bytes ssnd_body(uint32_t offset, const Bytes& pcm) {
    Bytes b;
    app_u32(b, offset);
    app_u32(b, 0);
    b.insert(b.end(), offset, 0);
    app_bytes(b, pcm);
    return b;
}

// Whole file: "FORM" size (ignored by the reader) + form type + chunks.
Bytes file(const char* form, const std::vector<Bytes>& chunks) {
    Bytes body;
    app(body, form);
    for (const Bytes& c : chunks) app_bytes(body, c);
    Bytes out;
    app(out, "FORM");
    app_u32(out, static_cast<uint32_t>(body.size()));  // FORM size: bytes after the size field
    app_bytes(out, body);
    return out;
}

// Plain AIFF with sample data already in big-endian order.
Bytes aiff_file(uint32_t channels, uint32_t frames, uint32_t bits, uint64_t rate, const Bytes& pcm,
                uint32_t offset = 0) {
    return file("AIFF", {chunk("COMM", comm_body(channels, frames, bits, ext80(rate))),
                         chunk("SSND", ssnd_body(offset, pcm))});
}

void put(const std::string& path, const Bytes& b) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

op::wav::ReadResult load(const Bytes& b) {
    put(tmp_file("op_aiff_case.aiff"), b);
    return op::wav::read_aiff(tmp_file("op_aiff_case.aiff"));
}

// Expected float for a 16-bit / 24-bit sample value (same expressions as the reader).
float f16(int v) { return static_cast<float>(v) * (1.0f / 32768.0f); }
float f24(int v) { return static_cast<float>(v) * (1.0f / 8388608.0f); }

Bytes be16(int v) { Bytes b; app_u16(b, static_cast<uint16_t>(v)); return b; }
Bytes le16(int v) { Bytes b; b.push_back(static_cast<uint8_t>(v)); b.push_back(static_cast<uint8_t>(v >> 8)); return b; }
Bytes be24(int v) { Bytes b; for (int s = 16; s >= 0; s -= 8) b.push_back(static_cast<uint8_t>(v >> s)); return b; }
Bytes le24(int v) { Bytes b; for (int s = 0; s <= 16; s += 8) b.push_back(static_cast<uint8_t>(v >> s)); return b; }

bool same_floats(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0) return false;
    return true;
}

}  // namespace aiff_t

static void test_aiff() {
    using namespace aiff_t;
    const int st16[] = {1000, -1000, 32767, -32768, 0, 1, -1, 12345};
    Bytes pcm16;
    std::vector<float> want16;
    for (int v : st16) { app_bytes(pcm16, be16(v)); want16.push_back(f16(v)); }

    // (a) 16-bit stereo and mono.
    {
        auto r = load(aiff_file(2, 4, 16, 48000, pcm16));
        CHECK(r.ok && r.sample_rate == 48000 && r.channels == 2 && r.format == "aiff" && same_floats(r.samples, want16),
              "aiff.16bit_stereo_exact");
        Bytes mono;
        for (int v : st16) app_bytes(mono, be16(v));
        auto m = load(aiff_file(1, 8, 16, 44100, mono));
        CHECK(m.ok && m.sample_rate == 44100 && m.channels == 1 && m.format == "aiff" && same_floats(m.samples, want16),
              "aiff.16bit_mono_exact");
    }
    // (b) 24-bit stereo, sign-extended.
    {
        const int st24[] = {0x123456, -0x123456, 8388607, -8388608, 0, 256, -256, 1};
        Bytes pcm24;
        std::vector<float> want24;
        for (int v : st24) { app_bytes(pcm24, be24(v)); want24.push_back(f24(v)); }
        auto r = load(aiff_file(2, 4, 24, 96000, pcm24));
        CHECK(r.ok && r.sample_rate == 96000 && r.channels == 2 && same_floats(r.samples, want24),
              "aiff.24bit_stereo_exact");
    }
    // (c) AIFF-C NONE (big-endian) and sowt (little-endian), 16 and 24-bit.
    {
        auto nb = load(file("AIFC", {chunk("COMM", comm_body(2, 4, 16, ext80(48000), "NONE")),
                                     chunk("SSND", ssnd_body(0, pcm16))}));
        CHECK(nb.ok && nb.format == "aiff" && same_floats(nb.samples, want16), "aiff.aifc_none_16");
        Bytes le;
        for (int v : st16) app_bytes(le, le16(v));
        auto sw = load(file("AIFC", {chunk("COMM", comm_body(2, 4, 16, ext80(48000), "sowt")),
                                     chunk("SSND", ssnd_body(0, le))}));
        CHECK(sw.ok && same_floats(sw.samples, want16), "aiff.aifc_sowt_16");
        const int st24[] = {0x7FFFFF, -0x7FFFFF, 0x000100, -0x000100, 0, 1, -1, 5};
        Bytes be24s, le24s;
        std::vector<float> want24;
        for (int v : st24) { app_bytes(be24s, be24(v)); app_bytes(le24s, le24(v)); want24.push_back(f24(v)); }
        auto n24 = load(file("AIFC", {chunk("COMM", comm_body(2, 4, 24, ext80(44100), "NONE")),
                                      chunk("SSND", ssnd_body(0, be24s))}));
        auto s24 = load(file("AIFC", {chunk("COMM", comm_body(2, 4, 24, ext80(44100), "sowt")),
                                      chunk("SSND", ssnd_body(0, le24s))}));
        CHECK(n24.ok && same_floats(n24.samples, want24) && s24.ok && same_floats(s24.samples, want24),
              "aiff.aifc_none_and_sowt_24");
    }
    // (d) 80-bit rate decode is exact for common rates.
    {
        const uint64_t rates[] = {8000, 11025, 22050, 44100, 48000, 88200, 96000, 192000};
        bool all = true;
        for (uint64_t hz : rates) {
            auto r = load(aiff_file(1, 1, 16, hz, be16(7)));
            all = all && r.ok && r.sample_rate == static_cast<int>(hz);
        }
        CHECK(all, "aiff.rate_decode_exact_common_rates");
        // Spec examples, byte-for-byte.
        CHECK(ext80(44100) == Bytes({0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0}),
              "aiff.ext80_encoder_matches_spec_44100");
    }
    // (e) one CHECK per error row (2-12), exact text.
    {
        const std::string nope = tmp_file("op_aiff_does_not_exist.aiff");
        auto r1 = op::wav::read_aiff(nope);
        CHECK(!r1.ok && r1.error == "cannot open '" + nope + "'" && r1.format == "aiff", "aiff.err_cannot_open");

        auto r2 = load(Bytes({'F', 'O', 'R', 'M', 0, 0, 0, 4, 'A', 'I', 'F'}));
        CHECK(!r2.ok && r2.error == "file too small for AIFF", "aiff.err_file_too_small");

        auto r3 = load(file("AIFF", {}));  // header only: no chunks
        Bytes bad_magic = aiff_file(1, 1, 16, 44100, be16(1));
        bad_magic[0] = 'X';
        Bytes bad_type = aiff_file(1, 1, 16, 44100, be16(1));
        bad_type[8] = 'W';
        auto r3a = load(bad_magic);
        auto r3b = load(bad_type);
        CHECK(!r3a.ok && r3a.error == "not a FORM/AIFF file" && !r3b.ok && r3b.error == "not a FORM/AIFF file" &&
                  r3.error == "missing COMM/SSND chunk",
              "aiff.err_not_form_aiff");

        auto r4 = load(file("AIFF", {chunk("COMM", Bytes(17, 0)), chunk("SSND", ssnd_body(0, be16(1)))}));
        auto r4c = load(file("AIFC", {chunk("COMM", Bytes(21, 0)), chunk("SSND", ssnd_body(0, be16(1)))}));
        CHECK(!r4.ok && r4.error == "COMM chunk too short" && !r4c.ok && r4c.error == "COMM chunk too short",
              "aiff.err_comm_too_short");

        auto r5 = load(file("AIFF", {chunk("COMM", comm_body(1, 1, 16, ext80(44100))), chunk("SSND", Bytes(7, 0))}));
        CHECK(!r5.ok && r5.error == "SSND chunk too short", "aiff.err_ssnd_too_short");

        auto r6 = load(aiff_file(1, 1, 16, 44100, be16(1), 0));
        // Offset one past the available bytes: body = 8 + 2 data bytes, offset 3 > 2.
        auto r6b = load(file("AIFF", {chunk("COMM", comm_body(1, 1, 16, ext80(44100))),
                                      chunk("SSND", [] { Bytes b; app_u32(b, 3); app_u32(b, 0); app_bytes(b, be16(1)); return b; }())}));
        CHECK(r6.ok && !r6b.ok && r6b.error == "truncated sound data", "aiff.err_truncated_offset");

        auto r7 = load(file("AIFF", {chunk("COMM", comm_body(1, 1, 16, ext80(44100)))}));
        Bytes cut = aiff_file(1, 1, 16, 44100, be16(1));
        cut.resize(cut.size() - 1);  // SSND declared size runs past EOF: scan stops, SSND not found
        auto r7b = load(cut);
        CHECK(!r7.ok && r7.error == "missing COMM/SSND chunk" && !r7b.ok && r7b.error == "missing COMM/SSND chunk",
              "aiff.err_missing_chunk");

        auto r8 = load(aiff_file(3, 1, 16, 44100, Bytes(6, 0)));
        auto r8b = load(aiff_file(0, 1, 16, 44100, be16(0)));
        CHECK(!r8.ok && r8.error == "only mono/stereo supported" && !r8b.ok && r8b.error == "only mono/stereo supported",
              "aiff.err_channels");

        auto r9 = load(file("AIFC", {chunk("COMM", comm_body(1, 1, 16, ext80(44100), "ULAW")),
                                     chunk("SSND", ssnd_body(0, be16(1)))}));
        CHECK(!r9.ok && r9.error == "unsupported AIFF-C compression", "aiff.err_aifc_compression");

        auto r10 = load(aiff_file(1, 1, 8, 44100, Bytes(1, 0)));
        auto r10b = load(aiff_file(1, 1, 32, 44100, Bytes(4, 0)));
        CHECK(!r10.ok && r10.error == "AIFF must be 16 or 24-bit" && !r10b.ok && r10b.error == "AIFF must be 16 or 24-bit",
              "aiff.err_sample_size");

        Bytes neg = ext80(44100);
        neg[0] |= 0x80;                                     // sign bit set
        Bytes inf = ext80(44100);
        inf[0] = 0x7F; inf[1] = 0xFF;                       // exponent 0x7FFF
        Bytes frac = {0x40, 0x0E, 0xAC, 0x44, 0, 0, 0, 0, 0, 0x01};  // 44100 with a fractional low bit
        Bytes big = ext80(1000001);
        Bytes zero = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const Bytes bad_rates[] = {neg, inf, frac, big, zero};
        bool rates_bad = true;
        for (const Bytes& rb : bad_rates) {
            auto r = load(file("AIFF", {chunk("COMM", comm_body(1, 1, 16, rb)), chunk("SSND", ssnd_body(0, be16(1)))}));
            rates_bad = rates_bad && !r.ok && r.error == "implausible sample rate";
        }
        CHECK(rates_bad, "aiff.err_implausible_rate");

        auto r12 = load(aiff_file(2, 100, 16, 44100, Bytes(4, 0)));
        CHECK(!r12.ok && r12.error == "truncated sound data", "aiff.err_truncated_sound_data");

        auto empty = load(aiff_file(2, 0, 16, 44100, Bytes()));
        CHECK(empty.ok && empty.samples.empty() && empty.channels == 2, "aiff.zero_frames_valid");
    }
    // (f) read_audio dispatches by signature, not extension.
    {
        put(tmp_file("op_aiff_sig_a.wav"), aiff_file(2, 4, 16, 48000, pcm16));
        auto a = op::wav::read_audio(tmp_file("op_aiff_sig_a.wav"));
        CHECK(a.ok && a.format == "aiff" && same_floats(a.samples, want16), "aiff.dispatch_aiff_in_wav_name");

        op::wav::StereoBuffer buf;
        buf.sample_rate = 44100;
        for (int i = 0; i < 64; ++i) buf.interleaved.push_back(static_cast<float>(i - 32) / 40.0f);
        std::string err;
        CHECK(op::wav::write_pcm16_stereo(tmp_file("op_aiff_sig_b.aiff"), buf, err), "aiff.dispatch_wav_write_ok");
        auto w = op::wav::read_audio(tmp_file("op_aiff_sig_b.aiff"));
        auto wd = op::wav::read_wav(tmp_file("op_aiff_sig_b.aiff"));
        CHECK(w.ok && w.format == "wav" && wd.ok && w.error == wd.error && w.sample_rate == wd.sample_rate &&
                  w.channels == wd.channels && same_floats(w.samples, wd.samples),
              "aiff.dispatch_wav_in_aiff_name_identical_to_read_wav");
        auto none = op::wav::read_audio(tmp_file("op_aiff_does_not_exist.wav"));
        auto none_wav = op::wav::read_wav(tmp_file("op_aiff_does_not_exist.wav"));
        CHECK(!none.ok && none.error == none_wav.error && none.format == "wav", "aiff.dispatch_unopenable_is_wav_error");
    }
    // (g) odd-sized unknown chunk (pad byte) before SSND, non-zero SSND offset.
    {
        const Bytes data = pcm16;
        Bytes name = chunk("NAME", Bytes({'x', 'y', 'z'}));  // size 3 → one pad byte
        Bytes ssnd = chunk("SSND", ssnd_body(6, data));     // 6 bytes of skipped data before the samples
        auto r = load(file("AIFF", {name, chunk("COMM", comm_body(2, 4, 16, ext80(48000))), ssnd}));
        CHECK(r.ok && same_floats(r.samples, want16), "aiff.odd_chunk_pad_and_ssnd_offset");
    }
    // (h) seeded structural fuzz, like test_wav_fuzz.
    {
        const Bytes seed = aiff_file(2, 16, 16, 48000, Bytes(64, 0x21));
        op::rng::Pcg32 prng(20261120ull);
        int survived = 0;
        bool sane = true;
        for (int trial = 0; trial < 400; ++trial) {
            Bytes m = seed;
            if (trial % 3 == 0) {
                m.resize(static_cast<size_t>(prng.next() % m.size()));
            } else {
                const int n_edits = 1 + static_cast<int>(prng.next() % 5);
                for (int e = 0; e < n_edits && !m.empty(); ++e) {
                    const size_t pos = static_cast<size_t>(prng.next() % m.size());
                    m[pos] = static_cast<uint8_t>(prng.next() % 256u);
                }
            }
            op::wav::ReadResult r;
            try {
                r = load(m);
            } catch (...) { r.ok = false; sane = false; }
            if (r.ok) {
                for (float s : r.samples)
                    if (!std::isfinite(s) || std::fabs(s) > 1.0f) sane = false;
                // Frames from the mutated COMM: walk chunks the way the reader does (first COMM).
                size_t frames = 0;
                bool found = false;
                size_t pos = 12;
                while (!found && pos + 8 <= m.size()) {
                    const uint32_t sz = (static_cast<uint32_t>(m[pos + 4]) << 24) | (static_cast<uint32_t>(m[pos + 5]) << 16) |
                                        (static_cast<uint32_t>(m[pos + 6]) << 8) | static_cast<uint32_t>(m[pos + 7]);
                    if (pos + 8 + static_cast<size_t>(sz) > m.size()) break;
                    if (std::memcmp(&m[pos], "COMM", 4) == 0) {
                        frames = (static_cast<size_t>(m[pos + 10]) << 24) | (static_cast<size_t>(m[pos + 11]) << 16) |
                                 (static_cast<size_t>(m[pos + 12]) << 8) | static_cast<size_t>(m[pos + 13]);
                        found = true;
                    }
                    pos += 8 + sz + (sz & 1u);
                }
                if (!found || r.samples.size() != frames * static_cast<size_t>(r.channels)) sane = false;
            }
            ++survived;
        }
        CHECK(survived == 400 && sane, "aiff.random_fuzz_survives_and_sane");
    }
}

// --- explicit pattern length (WP-0162, spec project-model.md §Explicit pattern length +
// operations.md §Pattern length operations): extent, render spacing, parse/save, ops, diff, steps.
static std::string run_cmd(const std::string& cmd) {  // capture stdout of a shell command
    std::string out;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return out;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
    ::pclose(pipe);
    return out;
}

static void test_pattern_length() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    // Per-test unique temp dir (pid-scoped): parallel test runs never collide on files.
    const fs::path dir = fs::path(tmp_file("op_wp0162"));
    std::error_code fec;
    fs::create_directories(dir, fec);
    std::string err;

    // pattern_extent helper: derived max(t+dur) when absent (0.0 for empty), explicit value wins.
    op::Pattern pe;
    CHECK(op::pattern_extent(pe) == 0.0, "plen.extent_empty_zero");
    pe.notes = {{0.0, 0.25}, {3.0, 0.25}, {1.0, 0.5}};
    CHECK(op::pattern_extent(pe) == 3.25, "plen.extent_derived_max");
    pe.length_beats = 4.0;
    CHECK(op::pattern_extent(pe) == 4.0, "plen.extent_explicit_wins");
    pe.length_beats = 1.5;  // shorter than the notes: still the extent (notes are never truncated)
    CHECK(op::pattern_extent(pe) == 1.5, "plen.extent_shorter_than_notes");

    // Base project: 1 sine track @120bpm/48kHz → 1 beat = 24000 samples; notes at beats 0..3 (dur .25).
    auto base_text = [&](const std::string& len_key) {  // len_key: "" or `,"length_beats":<json>`
        return std::string(R"({"format_version":0,"bpm":120,"length_bars":3,"sample_rate":48000,)"
                           R"("tracks":[{"name":"T","voice":{"kind":"sine","freq_hz":440.0}}],)"
                           R"("patterns":[{"track":0,"notes":[)"
                           R"({"t":0.0,"dur":0.25},{"t":1.0,"dur":0.25},{"t":2.0,"dur":0.25},{"t":3.0,"dur":0.25}])") +
                           len_key +
                           R"(}],"arrangement":[{"pattern":0,"start_bar":0.0,"repeats":3}]})";
    };
    const std::string e_len = "pattern.length_beats must be a finite number > 0";

    // (1) length_beats=4, repeats=3 → onsets at beats 0..11 (spacing 4), gaps inside each bar.
    op::Project pl;
    CHECK(parse_text(base_text(R"(,"length_beats":4)"), pl, err) && pl.patterns[0].length_beats &&
              *pl.patterns[0].length_beats == 4.0,
          "plen.parse_field");
    const auto mean_sq = [](const op::wav::StereoBuffer& b, long long a, long long z) {
        double s = 0.0;
        for (long long i = a * 2; i < z * 2 && i + 1 < static_cast<long long>(b.interleaved.size()); i += 2)
            s += double(b.interleaved[i]) * b.interleaved[i];
        return s / double(z - a);
    };
    const auto sound = [&](const op::wav::StereoBuffer& b, double beat) {
        return mean_sq(b, static_cast<long long>(beat * 24000) + 300,
                       static_cast<long long>(beat * 24000) + 5700) > 0.01;
    };
    op::wav::StereoBuffer bl;
    CHECK(op::render_project(pl, "", bl, err), "plen.render_with_length_ok");
    bool onsets_0_to_11 = true;
    for (int b = 0; b <= 11; ++b) onsets_0_to_11 = onsets_0_to_11 && sound(bl, static_cast<double>(b));
    CHECK(onsets_0_to_11, "plen.length_loops_on_the_bar_beats_0_to_11");
    CHECK(!sound(bl, 3.25) && !sound(bl, 7.25), "plen.length_gaps_inside_bar");

    // (2) same project WITHOUT the field: derived extent 3.25 → drifts to 3.25, 4.25, ...
    op::Project pd;
    CHECK(parse_text(base_text(""), pd, err) && !pd.patterns[0].length_beats, "plen.parse_absent");
    op::wav::StereoBuffer bd;
    CHECK(op::render_project(pd, "", bd, err), "plen.render_without_length_ok");
    CHECK(sound(bd, 3.25) && sound(bd, 4.25) && !sound(bd, 4.0), "plen.derived_loops_early_drifts");

    // (2b) MIDI export placement matches the renderer: length_beats=4 → onsets at beats 0..11.
    CHECK(op::save_midi((dir / "len.mid").string(), pl, err), "plen.midi_saved");
    {
        op::Project mid_back;
        CHECK(op::load_midi((dir / "len.mid").string(), mid_back, err), "plen.midi_reloaded");
        std::vector<double> onsets;
        for (const auto& q : mid_back.patterns)
            for (const auto& n : q.notes) onsets.push_back(n.t_beats);
        std::sort(onsets.begin(), onsets.end());
        bool beats_0_to_11 = onsets.size() == 12;
        for (size_t i = 0; i < onsets.size() && i < 12; ++i)
            beats_0_to_11 = beats_0_to_11 && std::fabs(onsets[i] - static_cast<double>(i)) < 1e-6;
        CHECK(beats_0_to_11, "plen.midi_onsets_match_renderer_beats_0_to_11");
    }

    // (3) a note past the length is kept, not truncated/wrapped: t=2.5 in a length_beats=2 pattern
    // with repeats=2 plays at beats 2.5 and 4.5 (= base + r*2 + 2.5).
    {
        json j = json::parse(R"({"format_version":0,"bpm":120,"length_bars":2,"sample_rate":48000,)"
                             R"("tracks":[{"name":"T","voice":{"kind":"sine","freq_hz":440.0}}],)"
                             R"("patterns":[{"track":0,"notes":[{"t":2.5,"dur":0.25}],"length_beats":2}],)"
                             R"("arrangement":[{"pattern":0,"start_bar":0.0,"repeats":2}]})");
        write_text((dir / "past.opj").string(), j.dump());
        op::Project pp;
        CHECK(op::load_project((dir / "past.opj").string(), pp, err), "plen.note_past_parse");
        op::wav::StereoBuffer bp;
        bool ok = op::render_project(pp, "", bp, err);
        CHECK(ok && sound(bp, 2.5) && sound(bp, 4.5) && !sound(bp, 0.5), "plen.note_past_length_kept");
    }

    // (4) validation errors, exact strings: 0, -1, string; check happens after the notes.
    auto expect_len_err = [&](const std::string& len_json, const char* name) {
        op::Project q;
        std::string e;
        const bool ok = parse_text(base_text("," + len_json), q, e);
        CHECK(!ok && e == e_len, name);
    };
    expect_len_err(R"("length_beats":0)", "plen.err_zero");
    expect_len_err(R"("length_beats":-1)", "plen.err_negative");
    expect_len_err(R"("length_beats":"4")", "plen.err_string");
    {  // inf/NaN are unspellable in JSON text — inject through the engine-internal parse
        json jinf = json::parse(base_text(R"(,"length_beats":4)"));
        jinf["patterns"][0]["length_beats"] = std::numeric_limits<double>::infinity();
        op::Project q;
        CHECK(!op::project_from_json(jinf, q, err) && err == e_len, "plen.err_not_finite");
    }
    {  // notes are validated first: bad note + bad length reports the note error
        op::Project q;
        std::string e;
        const bool ok = parse_text(
            R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],)"
            R"("patterns":[{"track":0,"notes":[{"t":-1,"dur":0.25}],"length_beats":0}]})", q, e);
        CHECK(!ok && e == "note needs t >= 0 and dur > 0", "plen.err_notes_checked_first");
    }

    // (5) canonical save: key omitted when absent; emitted as double before "notes"; round-trips.
    const std::string bytes_absent = op::project_bytes(pd);
    CHECK(bytes_absent.find("length_beats") == std::string::npos, "plen.save_omits_absent");
    const std::string bytes_len = op::project_bytes(pl);
    CHECK(bytes_len.find("\"length_beats\": 4.0") != std::string::npos &&
              bytes_len.find("\"length_beats\"") < bytes_len.find("\"notes\""),
          "plen.save_emits_double");
    op::Project back;
    CHECK(parse_text(bytes_len, back, err) && back.patterns[0].length_beats &&
              *back.patterns[0].length_beats == 4.0 && op::project_bytes(back) == bytes_len,
          "plen.save_roundtrip_stable");

    // (6) ops: set/clear with History undo/redo byte-exact; clear on absent is a no-op success.
    op::Project q;
    CHECK(parse_text(base_text(""), q, err), "plen.ops_base_parse");
    const std::string base_bytes = op::project_bytes(q);
    op::History h;
    CHECK(h.apply(q, json{{"op", "set_pattern_length"}, {"pattern", 0}, {"length_beats", 4.0}}, err) &&
              q.patterns[0].length_beats && *q.patterns[0].length_beats == 4.0,
          "plen.op_set");
    const std::string with_len = op::project_bytes(q);
    CHECK(h.apply(q, json{{"op", "clear_pattern_length"}, {"pattern", 0}}, err) &&
              !q.patterns[0].length_beats,
          "plen.op_clear");
    CHECK(op::project_bytes(q) == base_bytes, "plen.op_clear_bytes_match_base");
    CHECK(h.undo(q) && op::project_bytes(q) == with_len, "plen.undo_clear_restores_length");
    CHECK(h.undo(q) && op::project_bytes(q) == base_bytes, "plen.undo_set_restores_bytes");
    CHECK(h.redo(q) && op::project_bytes(q) == with_len, "plen.redo_set");
    CHECK(op::apply_op(q, json{{"op", "clear_pattern_length"}, {"pattern", 0}}, err), "plen.op_clear_noop_ok");

    // (7) op argument rules: exact error strings; positivity comes from the model parse.
    op::Project r;
    CHECK(parse_text(base_text(""), r, err), "plen.rules_base_parse");
    const std::string base_r = op::project_bytes(r);
    CHECK(!op::apply_op(r, json{{"op", "set_pattern_length"}, {"pattern", 0}}, err) &&
              err == "set_pattern_length: missing argument 'length_beats'", "plen.op_set_missing_len");
    CHECK(!op::apply_op(r, json{{"op", "set_pattern_length"}, {"pattern", 0}, {"length_beats", "4"}}, err) &&
              err == "set_pattern_length: 'length_beats' must be a number", "plen.op_set_not_number");
    CHECK(!op::apply_op(r, json{{"op", "set_pattern_length"}, {"pattern", 0}, {"length_beats", 0}}, err) &&
              err == "set_pattern_length: pattern.length_beats must be a finite number > 0",
          "plen.op_set_zero_rejected_by_parse");
    CHECK(!op::apply_op(r, json{{"op", "set_pattern_length"}, {"pattern", 1}, {"length_beats", 4}}, err) &&
              err == "set_pattern_length: 'pattern' out of range", "plen.op_set_pattern_oob");
    CHECK(!op::apply_op(r, json{{"op", "clear_pattern_length"}, {"pattern", 0}, {"x", 1}}, err) &&
              err == "clear_pattern_length: unknown argument 'x'", "plen.op_clear_unknown_arg");
    CHECK(op::project_bytes(r) == base_r, "plen.failed_ops_leave_project_untouched");

    // (8) opd diff step 7b, both directions + value change; identical projects emit nothing.
    op::Project da = q;  // no length
    op::Project db = q;
    db.patterns[0].length_beats = 4.0;
    op::Project dc = q;
    dc.patterns[0].length_beats = 2.0;
    auto diff_expect = [&](const op::Project& x, const op::Project& y, const json& want_first,
                           const char* name) {
        std::vector<json> ops;
        std::string e;
        if (!op::diff_projects(x, y, ops, e)) { CHECK(false, name); return; }
        bool first_ok = ops.size() == 1 && ops[0] == want_first;
        op::Project work = x;
        bool applied = true;
        for (const json& o : ops) applied = applied && op::apply_op(work, o, e);
        CHECK(first_ok && applied && op::project_bytes(work) == op::project_bytes(y), name);
    };
    diff_expect(da, db, json{{"op", "set_pattern_length"}, {"pattern", 0}, {"length_beats", 4.0}},
                "plen.diff_add");
    diff_expect(db, da, json{{"op", "clear_pattern_length"}, {"pattern", 0}}, "plen.diff_remove");
    diff_expect(db, dc, json{{"op", "set_pattern_length"}, {"pattern", 0}, {"length_beats", 2.0}},
                "plen.diff_change");
    std::vector<json> same_ops;
    CHECK(op::diff_projects(db, db, same_ops, err) && same_ops.empty(), "plen.diff_same_no_ops");

    // (9) `opd steps` default width: ceil(length_beats*4) clamped [1,256]; explicit --steps wins.
    const std::string opd = OPD_BIN;
    auto steps_out = [&](const std::string& opj, int pat, const std::string& extra) {
        return run_cmd("\"" + opd + "\" steps \"" + opj + "\" " + std::to_string(pat) + extra);
    };
    // length_beats=4 with notes at beats 0..3 → exactly the 16-step grid "x...x...x...x...".
    const fs::path proj_lb = dir / "steps.opj";
    write_text(proj_lb.string(), base_text(R"(,"length_beats":4)"));
    CHECK(steps_out(proj_lb.string(), 0, "") == "x...x...x...x...\n", "plen.steps_default_width_16");
    // ceil(8*4)=32 and ceil(0.5*4)=2: width follows the length; marks only where notes are on-grid.
    json j8 = json::parse(base_text(R"(,"length_beats":8)"));
    const fs::path proj8 = dir / "steps8.opj";
    write_text(proj8.string(), j8.dump());
    CHECK(steps_out(proj8.string(), 0, "") == "x...x...x...x..." + std::string(16, '.') + "\n",
          "plen.steps_width_32");
    json jhalf = json::parse(base_text(R"(,"length_beats":0.5)"));
    const fs::path proj_half = dir / "steps_half.opj";
    write_text(proj_half.string(), jhalf.dump());
    CHECK(steps_out(proj_half.string(), 0, "") == "x.\n", "plen.steps_width_2");
    // ceil(1e6*4) clamps to 256 columns; an explicit --steps always wins.
    json jbig = json::parse(base_text(R"(,"length_beats":1e6)"));
    const fs::path proj_big = dir / "steps_big.opj";
    write_text(proj_big.string(), jbig.dump());
    CHECK(steps_out(proj_big.string(), 0, "") == "x...x...x...x..." + std::string(240, '.') + "\n",
          "plen.steps_width_clamped_256");
    CHECK(steps_out(proj_lb.string(), 0, " --steps 8") == "x...x...\n", "plen.steps_explicit_wins");
    const fs::path proj_nolen = dir / "steps_nolen.opj";
    write_text(proj_nolen.string(), base_text(""));
    CHECK(steps_out(proj_nolen.string(), 0, "") == "x...x...x...x...\n", "plen.steps_default_16_without_field");
}

// ---------------------------------------------------------------- new_project factory (WP-0165)

static void test_new_project() {
    namespace fs = std::filesystem;
    std::string err;

    // Default spec: byte-exact canonical content fixed by docs/spec/project-model.md §New project.
    const op::Project def = op::new_project(op::NewProjectSpec{});
    const std::string expected = R"({
  "arrangement": [
    {
      "pattern": 0,
      "repeats": 1,
      "start_bar": 0.0
    }
  ],
  "beats_per_bar": 4,
  "bpm": 120.0,
  "format_version": 0,
  "length_bars": 4.0,
  "patterns": [
    {
      "notes": [],
      "track": 0
    }
  ],
  "sample_rate": 48000,
  "seed": 42,
  "tracks": [
    {
      "gain_db": 0.0,
      "name": "Synth",
      "pan": 0.0,
      "voice": {
        "attack_ms": 5.0,
        "kind": "synth",
        "release_ms": 30.0
      }
    }
  ]
}
)";
    CHECK(op::project_bytes(def) == expected, "new_project.default_bytes_exact");

    // Pure + deterministic: two calls with the same spec produce identical bytes.
    const op::Project def2 = op::new_project(op::NewProjectSpec{});
    CHECK(op::project_bytes(def) == op::project_bytes(def2), "new_project.deterministic");

    // 3-track spec (sine, sampler with path, metronome): loads back through the full validator.
    op::NewProjectSpec s;
    s.bpm = 140.0;
    s.bars = 8;
    s.beats_per_bar = 3;
    s.sample_rate = 44100;
    s.tracks = {{"Lead", op::VoiceKind::Sine, ""},
                {"Kick", op::VoiceKind::Sampler, "kits/kick.wav"},
                {"Click", op::VoiceKind::Metronome, ""}};
    const fs::path dir = fs::path(tmp_file("op_new_project"));
    fs::create_directories(dir);
    const std::string path = (dir / "three.opj").string();
    {  // write the factory's bytes verbatim, then load_project must accept them
        std::ofstream f(path, std::ios::binary);
        f << op::project_bytes(op::new_project(s));
    }
    op::Project p;
    CHECK(op::load_project(path, p, err), "new_project.three_track_loads");
    CHECK(p.tracks.size() == 3 && p.patterns.size() == 3 && p.arrangement.size() == 3,
          "new_project.three_track_sizes");
    bool wiring_ok = true;
    for (size_t i = 0; i < p.patterns.size(); ++i) {
        if (p.patterns[i].track != static_cast<int>(i)) wiring_ok = false;
        if (!p.patterns[i].notes.empty()) wiring_ok = false;
        if (p.arrangement[i].pattern != static_cast<int>(i) || p.arrangement[i].start_bar != 0.0 ||
            p.arrangement[i].repeats != 1)
            wiring_ok = false;
    }
    CHECK(wiring_ok, "new_project.pattern_i_references_track_i");
    bool voices_ok = p.tracks[0].voice.kind == op::VoiceKind::Sine && p.tracks[0].name == "Lead" &&
                     p.tracks[1].voice.kind == op::VoiceKind::Sampler &&
                     p.tracks[1].voice.path == "kits/kick.wav" &&  // stored verbatim
                     p.tracks[2].voice.kind == op::VoiceKind::Metronome &&
                     p.bpm == 140.0 && p.length_bars == 8.0 && p.beats_per_bar == 3 &&
                     p.sample_rate == 44100 && p.seed == 42 && p.format_version == 0;
    CHECK(voices_ok, "new_project.fields_and_sampler_path");
    fs::remove(path);
    fs::remove(dir);
}

// --- opd samples index (WP-0163): docs/spec/sample-index.md. Fixture bytes are explicit LE/BE assembly
// (mirrors the reader's expectations); the expected hashes below were computed with an independent
// FNV-1a implementation over those exact byte strings, so the JSON assertion is not self-referential.
namespace si_t {

std::string wav_pcm16(uint16_t channels, uint32_t rate, const std::vector<int16_t>& pcm) {
    auto u16 = [](std::string& s, uint16_t v) {
        s += static_cast<char>(v & 0xffu);
        s += static_cast<char>((v >> 8) & 0xffu);
    };
    auto u32 = [](std::string& s, uint32_t v) {
        for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xffu);
    };
    std::string data;
    for (const int16_t v : pcm) u16(data, static_cast<uint16_t>(v));
    const uint32_t block_align = static_cast<uint32_t>(channels) * 2u;
    std::string out;
    out += "RIFF";
    u32(out, 36u + static_cast<uint32_t>(data.size()));
    out += "WAVE";
    out += "fmt ";
    u32(out, 16);
    u16(out, 1);  // PCM
    u16(out, channels);
    u32(out, rate);
    u32(out, rate * block_align);
    u16(out, static_cast<uint16_t>(block_align));
    u16(out, 16);  // bits
    out += "data";
    u32(out, static_cast<uint32_t>(data.size()));
    out += data;
    return out;
}

void put(const std::filesystem::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace si_t

static void test_sample_index() {
    namespace fs = std::filesystem;
    using op::sample_index::build_sample_index;
    using op::sample_index::fnv1a64_hex;

    const unsigned char* empty = reinterpret_cast<const unsigned char*>("");
    CHECK(fnv1a64_hex(empty, 0) == "cbf29ce484222325", "samples.fnv1a_empty");
    const std::string one = "a";
    CHECK(fnv1a64_hex(reinterpret_cast<const unsigned char*>(one.data()), 1) == "af63dc4c8601ec8c",
          "samples.fnv1a_a");

    // Per-test unique temp dir (WP-0160 convention), removed at the end of this function.
    const fs::path root = fs::path(tmp_file("op_samples"));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "sub/deep");
    fs::create_directories(root / ".hiddendir");
    fs::create_directories(root / "empty");

    const std::string a_bytes = si_t::wav_pcm16(2, 48000, {16384, -16384, 8192, -8192});  // peak 0.5
    si_t::put(root / "a.wav", a_bytes);
    si_t::put(root / "UPPER.WAV", a_bytes);            // uppercase ext: same bytes → same hash
    si_t::put(root / "silence.wav", si_t::wav_pcm16(1, 48000, std::vector<int16_t>(48, 0)));
    si_t::put(root / ".hidden.wav", a_bytes);          // hidden file: skipped
    si_t::put(root / ".hiddendir/x.wav", a_bytes);     // hidden dir: not descended into
    si_t::put(root / "sub/deep/.keep", "x");           // hidden in nested dir: skipped
    si_t::put(root / "notes.txt", "hello");            // non-audio ext: ignored silently
    si_t::put(root / "bad.wav", "not a wav at all!!"); // 18 bytes < 44 → reader error, listed under errors

    aiff_t::Bytes pcm_be;
    for (const int v : {16384, -16384, 8192, -8192, 16384, -16384, 8192, -8192})  // 4 stereo frames
        aiff_t::app_u16(pcm_be, static_cast<uint16_t>(v));
    const aiff_t::Bytes b_aiff = aiff_t::aiff_file(2, 4, 16, 48000, pcm_be);
    si_t::put(root / "sub/b.aiff", std::string(b_aiff.begin(), b_aiff.end()));

    fs::create_symlink("a.wav", root / "link.wav", ec);   // symlink to file: skipped
    fs::create_directory_symlink("sub", root / "symdir", ec);  // symlink to dir: not followed

    std::string out, err;
    CHECK(build_sample_index(root.string(), out, err), "samples.tree_build_ok");

    // Byte-exact expected JSON (single line + trailing newline). Key order, no spaces, %.6f durations,
    // %.3f peak_dbfs (20*log10(0.5) = -6.021), null peak for silence, byte-wise sort
    // ("UPPER.WAV" < "a.wav" < "silence.wav" < "sub/b.aiff"), hidden entries absent, symlinks absent,
    // notes.txt absent, bad.wav under errors with the reader's verbatim text.
    const std::string want =
        "{\"version\":1,\"files\":["
        "{\"path\":\"UPPER.WAV\",\"format\":\"wav\",\"sample_rate\":48000,\"channels\":2,\"frames\":2,"
        "\"duration_s\":0.000042,\"peak_dbfs\":-6.021,\"hash\":\"5b4e7faeeeefe692\"},"
        "{\"path\":\"a.wav\",\"format\":\"wav\",\"sample_rate\":48000,\"channels\":2,\"frames\":2,"
        "\"duration_s\":0.000042,\"peak_dbfs\":-6.021,\"hash\":\"5b4e7faeeeefe692\"},"
        "{\"path\":\"silence.wav\",\"format\":\"wav\",\"sample_rate\":48000,\"channels\":1,\"frames\":48,"
        "\"duration_s\":0.001000,\"peak_dbfs\":null,\"hash\":\"5127a9f9cd28931f\"},"
        "{\"path\":\"sub/b.aiff\",\"format\":\"aiff\",\"sample_rate\":48000,\"channels\":2,\"frames\":4,"
        "\"duration_s\":0.000083,\"peak_dbfs\":-6.021,\"hash\":\"d1adb4fd9f17869e\"}"
        "],\"errors\":[{\"path\":\"bad.wav\",\"error\":\"file too small for canonical WAV\"}]}\n";
    CHECK(out == want, "samples.tree_exact_json");

    CHECK(out.find("link.wav") == std::string::npos && out.find("symdir") == std::string::npos &&
              out.find(".hidden") == std::string::npos && out.find("notes.txt") == std::string::npos,
          "samples.symlinks_hidden_and_nonaudio_absent");

    std::string empty_out;
    CHECK(build_sample_index((root / "empty").string(), empty_out, err) &&
              empty_out == "{\"version\":1,\"files\":[],\"errors\":[]}\n",
          "samples.empty_dir_arrays");

    const std::string twice_before = out;
    CHECK(build_sample_index(root.string(), out, err) && out == twice_before,
          "samples.determinism_identical_rerun");

    fs::remove_all(root, ec);

    // Nonexistent dir → false with err = the dir path (and nothing written to json_out by contract).
    const std::string nope = tmp_file("op_samples_missing");
    std::string e2;
    CHECK(!build_sample_index(nope, out, e2) && e2 == nope, "samples.nonexistent_dir_false");

    // A regular file passed as dir → false with err = the path, too.
    const fs::path not_a_dir = fs::path(tmp_file("op_samples_file"));
    si_t::put(not_a_dir, "x");
    std::string e3;
    CHECK(!build_sample_index(not_a_dir.string(), out, e3) && e3 == not_a_dir.string(),
          "samples.file_as_dir_false");
    fs::remove(not_a_dir, ec);
}

// ---------------------------------------------------------------- pattern ids + duplicate_pattern (WP-0179)

static void test_duplicate_pattern() {
    namespace fs = std::filesystem;
    using nlohmann::json;
    const fs::path dir = fs::path(tmp_file("op_wp0179"));
    std::error_code fec;
    fs::create_directories(dir, fec);
    std::string err;

    // Base project: pattern 0 rich (id "A", swing, length_beats, notes with note + vel),
    // pattern 1 bare. The single clip references pattern 0 only, so duplicating any pattern
    // must not change one sample of the render. Two tracks so a track difference is legal.
    const std::string pat_rich = R"({"track":0,"id":"A","swing":0.25,"length_beats":2.0,"notes":[)"
                                 R"({"t":0.0,"dur":0.25,"note":60,"vel":0.5},)"
                                 R"({"t":1.0,"dur":0.5,"note":64,"vel":1.0}]})";
    const std::string pat_bare = R"({"track":0,"notes":[{"t":0.0,"dur":0.25}]})";
    auto base_text = [&](const std::string& pats) {
        return std::string(R"({"format_version":0,"bpm":120,"length_bars":2,"sample_rate":48000,)"
                           R"("tracks":[{"name":"T","voice":{"kind":"sine","freq_hz":440.0}},)"
                           R"({"name":"U","voice":{"kind":"sine","freq_hz":660.0}}],)"
                           R"("patterns":[)") +
               pats + R"(],"arrangement":[{"pattern":0,"start_bar":0.0,"repeats":2}]})";
    };
    const std::string base = base_text(pat_rich + "," + pat_bare);

    // (1) parse stores the id; an unnamed pattern keeps the empty string.
    op::Project p;
    CHECK(parse_text(base, p, err) && p.patterns.size() == 2 && p.patterns[0].id == "A" &&
              p.patterns[1].id.empty(),
          "dup.parse_stores_id");

    // (2) canonical save writes "id" and it lands first among the pattern keys (alphabetical dump).
    const std::string bytes = op::project_bytes(p);
    CHECK(bytes.find("\"id\": \"A\"") != std::string::npos &&
              bytes.find("\"id\"") < bytes.find("\"length_beats\"") &&
              bytes.find("\"length_beats\"") < bytes.find("\"notes\""),
          "dup.save_id_first_key");

    // (3) canonical save omits the key when unnamed → legacy bytes unchanged.
    op::Project pu;
    CHECK(parse_text(base_text(pat_bare + "," + pat_bare), pu, err) &&
              op::project_bytes(pu).find("\"id\"") == std::string::npos,
          "dup.save_omits_unnamed");

    // (4) parse validation, exact strings, first failure wins.
    auto expect_parse_err = [&](const std::string& pats, const std::string& want) {
        op::Project q;
        std::string e;
        const bool ok = parse_text(base_text(pats), q, e);
        return !ok && e == want;
    };
    CHECK(expect_parse_err(R"({"track":0,"id":5,"notes":[]})", "pattern.id must be a string"),
          "dup.err_id_number");
    CHECK(expect_parse_err(R"({"track":0,"id":null,"notes":[]})", "pattern.id must be a string"),
          "dup.err_id_null");
    CHECK(expect_parse_err(R"({"track":0,"id":true,"notes":[]})", "pattern.id must be a string"),
          "dup.err_id_bool");
    CHECK(expect_parse_err(R"({"track":0,"id":{},"notes":[]})", "pattern.id must be a string"),
          "dup.err_id_object");
    CHECK(expect_parse_err(R"({"track":0,"id":[],"notes":[]})", "pattern.id must be a string"),
          "dup.err_id_array");
    CHECK(expect_parse_err(R"({"track":0,"id":"","notes":[]})", "pattern.id must not be empty"),
          "dup.err_id_empty");
    const std::string id64(64, 'x'), id65(65, 'x');
    CHECK(expect_parse_err("{\"track\":0,\"id\":\"" + id65 + "\",\"notes\":[]}",
                           "pattern.id must be at most 64 bytes"),
          "dup.err_id_65_bytes");
    op::Project p64;
    CHECK(parse_text(base_text("{\"track\":0,\"id\":\"" + id64 + "\",\"notes\":[]}"), p64, err) &&
              p64.patterns[0].id == id64,
          "dup.parse_id_64_bytes_ok");
    CHECK(expect_parse_err(pat_rich + "," + pat_rich, "pattern.id 'A' is already in use"),
          "dup.err_duplicate_ids_in_file");  // the later pattern fails (array order)
    // id rules come right after track and before swing: doubly-invalid patterns prove the order.
    CHECK(expect_parse_err(R"({"track":0,"id":5,"swing":9.0,"notes":[]})", "pattern.id must be a string"),
          "dup.err_id_before_swing");
    CHECK(expect_parse_err(R"({"track":7,"id":5,"notes":[]})", "pattern.track out of range: 7"),
          "dup.err_track_before_id");

    // (5) duplicate_pattern appends a deep copy; source, clips and indices untouched.
    op::Project q;
    CHECK(parse_text(base, q, err), "dup.ops_base_parse");
    const json src0 = op::project_to_json(q)["patterns"][0];
    CHECK(op::apply_op(q, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", "Copy"}}, err) &&
              q.patterns.size() == 3 && q.arrangement.size() == 1,
          "dup.op_appends_no_clip_added");
    CHECK(op::project_to_json(q)["patterns"][0] == src0, "dup.op_source_untouched");
    CHECK(q.patterns[2].id == "Copy" && q.patterns[2].track == 0 && q.patterns[2].swing == 0.25 &&
              q.patterns[2].length_beats && *q.patterns[2].length_beats == 2.0,
          "dup.op_copy_scalars_match");
    bool notes_copied = q.patterns[2].notes.size() == q.patterns[0].notes.size();
    if (notes_copied)
        for (size_t i = 0; i < q.patterns[0].notes.size(); ++i) {
            const op::Note& n0 = q.patterns[0].notes[i];
            const op::Note& n2 = q.patterns[2].notes[i];
            notes_copied = notes_copied && n2.t_beats == n0.t_beats && n2.dur_beats == n0.dur_beats &&
                           n2.midi_note == n0.midi_note && n2.velocity == n0.velocity;
        }
    CHECK(notes_copied, "dup.op_copy_notes_field_by_field");

    // (6) the resulting bytes equal those of a hand-built expected project.
    const std::string pat_copy = R"({"track":0,"id":"Copy","swing":0.25,"length_beats":2.0,"notes":[)"
                                 R"({"t":0.0,"dur":0.25,"note":60,"vel":0.5},)"
                                 R"({"t":1.0,"dur":0.5,"note":64,"vel":1.0}]})";
    op::Project want;
    CHECK(parse_text(base_text(pat_rich + "," + pat_bare + "," + pat_copy), want, err) &&
              op::project_bytes(q) == op::project_bytes(want),
          "dup.bytes_match_handbuilt_project");

    // (7) a copy of a bare pattern gains no swing/length_beats — only the id key.
    op::Project qb;
    CHECK(parse_text(base, qb, err) &&
              op::apply_op(qb, json{{"op", "duplicate_pattern"}, {"src", 1}, {"new_id", "Bare"}}, err) &&
              qb.patterns.size() == 3 && !qb.patterns[2].length_beats && qb.patterns[2].swing == 0.0,
          "dup.op_bare_copy_no_extras");
    {
        const json jb = op::project_to_json(qb)["patterns"][2];
        CHECK(!jb.contains("swing") && !jb.contains("length_beats") && jb.at("id") == "Bare",
              "dup.op_bare_copy_json_keys");
    }

    // (8) History: undo restores the exact original bytes, redo re-applies them byte-exactly;
    // a rejected apply changes neither the bytes nor the undo depth.
    op::Project hp;
    CHECK(parse_text(base, hp, err), "dup.history_base_parse");
    const std::string bytes0 = op::project_bytes(hp);
    op::History h;
    CHECK(h.apply(hp, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", "Copy"}}, err) &&
              h.undo_depth() == 1,
          "dup.history_apply");
    const std::string bytes1 = op::project_bytes(hp);
    CHECK(bytes1 != bytes0 && h.undo(hp) && op::project_bytes(hp) == bytes0,
          "dup.history_undo_byte_exact");
    CHECK(h.redo(hp) && op::project_bytes(hp) == bytes1, "dup.history_redo_byte_exact");
    CHECK(!h.apply(hp, json{{"op", "duplicate_pattern"}, {"src", 9}, {"new_id", "Copy"}}, err) &&
              op::project_bytes(hp) == bytes1 && h.undo_depth() == 1,
          "dup.history_rejected_no_change");

    // (9) every op rejection with its exact message; the project is untouched each time.
    auto rejected_unchanged = [&](op::Project& p2, const json& o, const std::string& want) {
        const std::string before = op::project_bytes(p2);
        std::string e;
        const bool ok = op::apply_op(p2, o, e);
        return !ok && e == want && op::project_bytes(p2) == before;
    };
    op::Project r;
    CHECK(parse_text(base, r, err), "dup.rules_base_parse");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"new_id", "X"}},
                             "duplicate_pattern: missing argument 'src'"),
          "dup.op_missing_src");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0.0}, {"new_id", "X"}},
                             "duplicate_pattern: 'src' must be an integer"),
          "dup.op_src_float");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", "0"}, {"new_id", "X"}},
                             "duplicate_pattern: 'src' must be an integer"),
          "dup.op_src_string");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", -1}, {"new_id", "X"}},
                             "duplicate_pattern: 'src' out of range"),
          "dup.op_src_negative");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 2}, {"new_id", "X"}},
                             "duplicate_pattern: 'src' out of range"),
          "dup.op_src_out_of_range");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0}},
                             "duplicate_pattern: missing argument 'new_id'"),
          "dup.op_missing_new_id");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", 1}},
                             "duplicate_pattern: 'new_id' must be a string"),
          "dup.op_new_id_number");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", nullptr}},
                             "duplicate_pattern: 'new_id' must be a string"),
          "dup.op_new_id_null");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", true}},
                             "duplicate_pattern: 'new_id' must be a string"),
          "dup.op_new_id_bool");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"x", 1}, {"src", 0}, {"new_id", "X"}},
                             "duplicate_pattern: unknown argument 'x'"),
          "dup.op_unknown_key");
    // check order is normative: keys before src, src before new_id.
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"x", 1}},
                             "duplicate_pattern: unknown argument 'x'"),
          "dup.op_unknown_key_before_src");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 9}},
                             "duplicate_pattern: 'src' out of range"),
          "dup.op_src_checked_before_new_id");
    // the id rules come from the model parse, prefixed by the op name:
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", ""}},
                             "duplicate_pattern: pattern.id must not be empty"),
          "dup.op_empty_new_id");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", id65}},
                             "duplicate_pattern: pattern.id must be at most 64 bytes"),
          "dup.op_new_id_65_bytes");
    CHECK(rejected_unchanged(r, json{{"op", "duplicate_pattern"}, {"src", 0}, {"new_id", "A"}},
                             "duplicate_pattern: pattern.id 'A' is already in use"),
          "dup.op_taken_id_source_own");

    // (10) add_pattern may carry an id: fresh accepted, duplicate refused by the model parse.
    CHECK(op::apply_op(r, json{{"op", "add_pattern"},
                               {"pattern", json{{"track", 0}, {"id", "Fresh"}, {"notes", json::array()}}}},
                       err) &&
              r.patterns.size() == 3 && r.patterns[2].id == "Fresh",
          "dup.add_pattern_fresh_id_ok");
    CHECK(rejected_unchanged(r, json{{"op", "add_pattern"},
                                     {"pattern", json{{"track", 0}, {"id", "A"}, {"notes", json::array()}}}},
                             "add_pattern: pattern.id 'A' is already in use"),
          "dup.add_pattern_taken_id_rejected");

    // (11) rendering before/after a duplicate is sample-identical (the only clip uses pattern 0):
    // base_p has the two original patterns, q additionally carries the appended copy.
    op::Project base_p;
    CHECK(parse_text(base, base_p, err), "dup.render_base_parse");
    op::wav::StereoBuffer buf_a, buf_b;
    CHECK(op::render_project(base_p, "", buf_a, err), "dup.render_before_ok");
    CHECK(op::render_project(q, "", buf_b, err) && buf_a.interleaved == buf_b.interleaved,
          "dup.render_sample_identical_after_duplicate");

    // (12) opd diff: base_p and q = base_p + copy → exactly one add_pattern carrying the id.
    std::vector<json> dops;
    const json want_add{{"op", "add_pattern"}, {"pattern", op::project_to_json(q)["patterns"][2]}};
    CHECK(op::diff_projects(base_p, q, dops, err) && dops.size() == 1 && dops[0] == want_add &&
              journal_text(dops).find("duplicate_pattern") == std::string::npos &&
              journal_text(dops).find("\"id\":\"Copy\"") != std::string::npos,
          "dup.diff_one_add_pattern_with_id");
    op::Project replay = base_p;
    CHECK(op::apply_journal(replay, journal_text(dops), err) &&
              op::project_bytes(replay) == op::project_bytes(q),
          "dup.diff_journal_replays_target_bytes");
    std::vector<json> same_ops;
    CHECK(op::diff_projects(base_p, base_p, same_ops, err) && same_ops.empty(), "dup.diff_equal_empty");
    std::vector<json> back_ops;
    const json want_rm{{"op", "remove_pattern"}, {"index", 2}};
    CHECK(op::diff_projects(q, base_p, back_ops, err) && back_ops.size() == 1 && back_ops[0] == want_rm,
          "dup.diff_reverse_single_remove_pattern");

    // (13) an id difference on a shared pattern is unsupported; track wins when both differ.
    op::Project renamed = q;
    renamed.patterns[0].id = "Renamed";
    std::vector<json> uid_ops;
    CHECK(!op::diff_projects(q, renamed, uid_ops, err) && uid_ops.empty() &&
              err == "unsupported difference: patterns[0].id",
          "dup.diff_unsupported_id");
    op::Project gained = q;
    gained.patterns[1].id = "New";  // present-vs-absent on a shared pattern differs too
    std::vector<json> gap_ops;
    CHECK(!op::diff_projects(q, gained, gap_ops, err) && gap_ops.empty() &&
              err == "unsupported difference: patterns[1].id",
          "dup.diff_unsupported_id_absent_vs_present");
    op::Project both = q;
    both.patterns[0].track = 1;
    both.patterns[0].id = "Renamed";
    std::vector<json> bt_ops;
    CHECK(!op::diff_projects(q, both, bt_ops, err) && bt_ops.empty() &&
              err == "unsupported difference: patterns[0].track",
          "dup.diff_track_reported_before_id");

    fs::remove_all(dir, fec);
}


// WP-0153: dz::Player driven by a fake device (plain process() calls on this thread) plus
// dz::format_position on the engine's tempo map. Buffers are ramps so copy/position bugs show up.
static std::unique_ptr<op::wav::StereoBuffer> make_ramp_buffer(long long frames, float base) {
    auto buf = std::make_unique<op::wav::StereoBuffer>();
    buf->interleaved.resize(static_cast<size_t>(frames) * 2);
    for (long long i = 0; i < frames; ++i) {
        buf->interleaved[static_cast<size_t>(2 * i)] = base + static_cast<float>(i);
        buf->interleaved[static_cast<size_t>(2 * i + 1)] = -base - static_cast<float>(i);
    }
    return buf;
}

static bool all_zero(const float* a, int n) {
    for (int i = 0; i < n; ++i) if (a[i] != 0.0f) return false;
    return true;
}

static void test_player() {
    using dz::Player;
    constexpr int kBlock = 64;
    float left[kBlock], right[kBlock];

    // Silence without a buffer, even while playing; position stays home.
    Player p;
    p.play();
    std::fill(left, left + kBlock, 1.0f);
    std::fill(right, right + kBlock, 1.0f);
    p.process(left, right, kBlock);
    CHECK(all_zero(left, kBlock) && all_zero(right, kBlock) && p.position_frames() == 0 &&
              p.frames_played() == 0,
          "player.silence_without_buffer");

    // Silence when not playing (buffer submitted but play() never called).
    Player q;
    q.submit(make_ramp_buffer(200, 1.0f));
    std::fill(left, left + kBlock, 1.0f);
    std::fill(right, right + kBlock, 1.0f);
    q.process(left, right, kBlock);
    CHECK(all_zero(left, kBlock) && all_zero(right, kBlock) && !q.playing() &&
              q.position_frames() == 0,
          "player.silence_when_not_playing");

    // Copy + position advance: block 1 adopts the buffer and plays frames 0..63.
    q.play();
    q.process(left, right, kBlock);
    bool copy_ok = true;
    for (int i = 0; i < kBlock; ++i) {
        if (left[i] != 1.0f + static_cast<float>(i) || right[i] != -1.0f - static_cast<float>(i)) copy_ok = false;
    }
    CHECK(copy_ok && q.position_frames() == kBlock && q.frames_played() == kBlock, "player.copy_and_position_advance");

    // Buffer swap keeps the position: this block adopts the second buffer and continues at 64.
    q.submit(make_ramp_buffer(200, 1000.0f));
    q.process(left, right, kBlock);
    bool swap_ok = true;
    for (int i = 0; i < kBlock; ++i) {
        if (left[i] != 1064.0f + static_cast<float>(i) || right[i] != -1064.0f - static_cast<float>(i)) swap_ok = false;
    }
    CHECK(swap_ok && q.position_frames() == 2 * kBlock, "player.swap_keeps_position");

    // Stop resets the position (immediately visible) and silences output.
    q.stop();
    const bool stopped_state = !q.playing() && q.position_frames() == 0;
    std::fill(left, left + kBlock, 1.0f);
    std::fill(right, right + kBlock, 1.0f);
    q.process(left, right, kBlock);
    CHECK(stopped_state && all_zero(left, kBlock) && all_zero(right, kBlock) && q.position_frames() == 0,
          "player.stop_resets_position_and_silences");

    // Restart from zero: play() after stop() starts at frame 0 of the current buffer.
    q.play();
    q.process(left, right, 16);
    CHECK(left[0] == 1000.0f && left[15] == 1015.0f && q.position_frames() == 16, "player.restart_from_zero");

    // End of song: partial block then zeros; playing() false and position parked at 0.
    Player e;
    e.submit(make_ramp_buffer(10, 5.0f));
    e.play();
    float el[8], er[8];
    e.process(el, er, 8);  // frames 0..7, song not over yet
    bool partial_ok = el[0] == 5.0f && el[7] == 12.0f && er[0] == -5.0f && e.position_frames() == 8 && e.playing();
    std::fill(el, el + 8, 1.0f);
    std::fill(er, er + 8, 1.0f);
    e.process(el, er, 8);  // frames 8..9 then zeros; end of song fires
    bool end_ok = partial_ok && el[0] == 13.0f && el[1] == 14.0f && el[2] == 0.0f && el[7] == 0.0f &&
                  er[0] == -13.0f && er[2] == 0.0f && !e.playing() && e.position_frames() == 0 &&
                  e.frames_played() == 10;
    CHECK(end_ok, "player.end_of_song_partial_then_zeros");

    // right == nullptr: mono output still copies the left channel and advances.
    Player m;
    m.submit(make_ramp_buffer(32, 7.0f));
    m.play();
    std::fill(left, left + kBlock, 1.0f);
    m.process(left, nullptr, 16);
    bool mono_ok = true;
    for (int i = 0; i < 16; ++i) if (left[i] != 7.0f + static_cast<float>(i)) mono_ok = false;
    CHECK(mono_ok && m.position_frames() == 16, "player.right_null_mono_output");

    // format_position on the default Project (120 bpm, 4/4, 48 kHz → 96000 frames/bar, 4 bars).
    const op::Project proj;
    CHECK(dz::format_position(proj, 0) == "1.1", "format_position.frame_0_is_1.1");
    CHECK(dz::format_position(proj, 23999) == "1.1" && dz::format_position(proj, 24000) == "1.2",
          "format_position.beat_boundary");
    CHECK(dz::format_position(proj, 95999) == "1.4" && dz::format_position(proj, 96000) == "2.1",
          "format_position.bar_boundary");
    CHECK(dz::format_position(proj, 4 * 96000LL - 1) == "4.4" && dz::format_position(proj, 1LL << 40) == "4.4" &&
              dz::format_position(proj, -7) == "1.1",
          "format_position.clamps_past_end");
}

// --- Clip fades (WP-0178): docs/spec/project-model.md §Clip fades + operations.md §Clip fade operation.
// Parse/validate/save, render-time linear-ramp bit-exactness vs the spec formula, per-clip & per-repeat
// scoping, tempo-map bounds via TempoMap::at, set_clip_fades op (set/null/zeros/reject), History undo/redo,
// and opd diff emission. All numbers pinned by the two spec sections.

// Write a stereo float32 asset of `frames` samples, both channels exactly 0.25f (native 48 kHz).
static void write_const_asset(const std::string& path, long long frames) {
    op::wav::StereoBuffer b;
    b.sample_rate = 48000;
    b.interleaved.assign(static_cast<size_t>(frames) * 2u, 0.25f);
    std::string e;
    op::wav::write_wav_stereo(path, b, op::wav::SampleFormat::F32, e);
}

// A hard-left (pan -1 → gain_l == 1.0f exactly) single-sampler-track project, one pattern with a single
// long note and explicit length_beats = `extent`, one clip [start_bar, repeats] with the given fades.
static op::Project fade_probe_project(const std::string& asset, double extent, int reps,
                                      double fade_in, double fade_out, double start_bar = 0.0,
                                      double note_dur = 16.0) {
    op::Project p;
    p.bpm = 120.0; p.length_bars = 8.0; p.beats_per_bar = 4; p.sample_rate = 48000; p.seed = 3;
    op::Track tr; tr.name = "K"; tr.gain_db = 0.0f; tr.pan = -1.0f;
    tr.voice.kind = op::VoiceKind::Sampler; tr.voice.path = asset;
    p.tracks = {tr};
    op::Pattern pat; pat.track = 0; pat.length_beats = extent;
    pat.notes.push_back({0.0, note_dur, -1, 1.0});  // long note covers every probed frame (unless shortened)
    p.patterns = {pat};
    op::Clip c; c.pattern = 0; c.start_bar = start_bar; c.repeats = reps;
    c.fade_in_beats = fade_in; c.fade_out_beats = fade_out;
    p.arrangement = {c};
    return p;
}

static void test_clip_fades() {
    std::string err;
    const std::string asset = tmp_file("op_cf_asset.wav");
    write_const_asset(asset, 384000);  // long enough for every probe below

    // --- Parse: defaults and explicit values.
    write_text(tmp_file("op_cf_p.opj"), R"({"format_version":0,"bpm":120,"length_bars":4,
        "tracks":[{"voice":{"kind":"sine","freq_hz":220}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}],
        "arrangement":[{"pattern":0},{"pattern":0,"fade_in_beats":1.5,"fade_out_beats":0.25}]})");
    op::Project p;
    CHECK(op::load_project(tmp_file("op_cf_p.opj"), p, err) && p.arrangement.size() == 2 &&
          p.arrangement[0].fade_in_beats == 0.0 && p.arrangement[0].fade_out_beats == 0.0 &&
          p.arrangement[1].fade_in_beats == 1.5 && p.arrangement[1].fade_out_beats == 0.25,
          "cf.parse_defaults_and_explicit");

    // --- Parse rejections: exact message per field and bad kind (fade-in first).
    auto reject_fade = [&](const char* arr_json, const std::string& want_msg, const char* name) {
        write_text(tmp_file("op_cf_r.opj"),
                   std::string(R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],)"
                               R"("patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}],"arrangement":)") + arr_json + "}");
        op::Project pb;
        std::string e;
        CHECK(!op::load_project(tmp_file("op_cf_r.opj"), pb, e) && e == want_msg, name);
    };
    reject_fade(R"([{"pattern":0,"fade_in_beats":-0.5}])", "clip.fade_in_beats must be a number in [0, 4096]", "cf.rejects_fade_in_negative");
    reject_fade(R"([{"pattern":0,"fade_in_beats":4096.5}])", "clip.fade_in_beats must be a number in [0, 4096]", "cf.rejects_fade_in_above_max");
    reject_fade(R"([{"pattern":0,"fade_in_beats":"0.5"}])", "clip.fade_in_beats must be a number in [0, 4096]", "cf.rejects_fade_in_string");
    reject_fade(R"([{"pattern":0,"fade_in_beats":null}])", "clip.fade_in_beats must be a number in [0, 4096]", "cf.rejects_fade_in_null");
    // fade-out-only failure with a valid fade-in (proves fade-in checked first, then fade-out):
    reject_fade(R"([{"pattern":0,"fade_in_beats":1.0,"fade_out_beats":-3.0}])", "clip.fade_out_beats must be a number in [0, 4096]", "cf.rejects_fade_out_negative");
    reject_fade(R"([{"pattern":0,"fade_in_beats":1.0,"fade_out_beats":true}])", "clip.fade_out_beats must be a number in [0, 4096]", "cf.rejects_fade_out_bool");

    // -0.0 accepted and equals 0; canonical save then omits both keys (never-faded bytes).
    write_text(tmp_file("op_cf_neg.opj"), R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}],
        "arrangement":[{"pattern":0,"fade_in_beats":-0.0,"fade_out_beats":-0.0}]})");
    op::Project pneg;
    CHECK(op::load_project(tmp_file("op_cf_neg.opj"), pneg, err) && pneg.arrangement[0].fade_in_beats == 0.0 &&
          pneg.arrangement[0].fade_out_beats == 0.0, "cf.neg_zero_accepted_as_zero");

    // --- Canonical save: emit when set (alphabetically before gain_db), omit at 0; round-trip.
    auto saved = [](const op::Project& q) { return saved_text(q); };
    std::string faded_txt = saved(p);  // p.arrangement[1] carries fades
    CHECK(faded_txt.find("\"fade_in_beats\": 1.5") != std::string::npos &&
          faded_txt.find("\"fade_out_beats\": 0.25") != std::string::npos, "cf.save_emits_fades");
    // Ordering within a clip that has both fades and a non-zero gain_db: fade keys land before gain_db.
    {
        op::Project ord = p;
        ord.arrangement[1].gain_db = -6.0;  // same clip: fade_in, fade_out, gain_db all present
        std::string t = saved(ord);
        const size_t fi = t.find("\"fade_in_beats\"");
        const size_t fo = t.find("\"fade_out_beats\"");
        const size_t gd = t.rfind("\"gain_db\"");  // last gain_db is this clip's (track has none at default)
        CHECK(fi != std::string::npos && fi < fo && fo < gd, "cf.save_fades_before_gain_db");
    }
    {
        std::string se;
        bool sv = op::save_project(tmp_file("op_cf_rt.opj"), p, se);
        op::Project rt;
        CHECK(sv && op::load_project(tmp_file("op_cf_rt.opj"), rt, err) &&
              rt.arrangement[1].fade_in_beats == 1.5 && rt.arrangement[1].fade_out_beats == 0.25,
              "cf.roundtrip_preserves_fades");
    }
    // A never-faded clip's saved bytes contain no fade key at all.
    CHECK(saved(pneg).find("fade_in_beats") == std::string::npos &&
          saved(pneg).find("fade_out_beats") == std::string::npos, "cf.save_omits_at_zero");

    // --- Render: fade-in region matches the spec formula bit-exactly; first frame 0; plateau 0.25f.
    auto render_L = [&](const op::Project& q) {
        op::wav::StereoBuffer out;
        std::string e;
        if (!op::render_project(q, "", out, e)) return std::vector<float>{};
        std::vector<float> L(out.interleaved.size() / 2);
        for (size_t i = 0; i < L.size(); ++i) L[i] = out.interleaved[i * 2];
        return L;
    };
    const long long SPB = 24000;  // 120 bpm @ 48 kHz, constant tempo: at(b) == llround(b*SPB)
    // Probe A: fade-in only over 1 beat on a clip [bar0, reps1, extent2]. Region [0, SPB).
    {
        op::Project q = fade_probe_project(asset, 2.0, 1, 1.0, 0.0);
        std::vector<float> L = render_L(q);
        CHECK(L.size() >= static_cast<size_t>(3 * SPB), "cf.A_rendered");
        bool ok_first = (L[0] == 0.0f);
        bool ok_ramp = true;
        for (long long f = 1; f < SPB; ++f) {
            const float g = static_cast<float>(static_cast<double>(f) / static_cast<double>(SPB));
            if (!(L[static_cast<size_t>(f)] == 0.25f * g)) { ok_ramp = false; break; }
        }
        bool ok_plateau = (L[static_cast<size_t>(SPB + 100)] == 0.25f) &&
                          (L[static_cast<size_t>(2 * SPB)] == 0.25f);  // plateau AND past clip end (not gated)
        CHECK(ok_first, "cf.A.first_frame_zero");
        CHECK(ok_ramp, "cf.A.fade_in_bit_exact");
        CHECK(ok_plateau, "cf.A.plateau_and_past_end_unity");
    }
    // Probe B: fade-out only over 1 beat ending at clip end (extent2). Region [SPB, 2*SPB); past → unity.
    {
        op::Project q = fade_probe_project(asset, 2.0, 1, 0.0, 1.0);
        std::vector<float> L = render_L(q);
        bool ok_plateau_before = (L[static_cast<size_t>(SPB - 1)] == 0.25f);
        bool ok_ramp = true;
        for (long long f = SPB; f < 2 * SPB; ++f) {
            const float g = static_cast<float>(1.0 - static_cast<double>(f - SPB) / static_cast<double>(SPB));
            if (!(L[static_cast<size_t>(f)] == 0.25f * g)) { ok_ramp = false; break; }
        }
        bool ok_past_end = (L[static_cast<size_t>(2 * SPB)] == 0.25f);  // B+M: gain back to 1 (overhang not gated)
        CHECK(ok_plateau_before, "cf.B.plateau_before_fade_out");
        CHECK(ok_ramp, "cf.B.fade_out_bit_exact");
        CHECK(ok_past_end, "cf.B.past_clip_end_not_gated");
    }
    // Probe C: explicit 0.0/0.0 renders bit-identically to keys absent (default path untouched).
    {
        op::Project with_zero = fade_probe_project(asset, 2.0, 1, 0.0, 0.0);
        op::Project none = fade_probe_project(asset, 2.0, 1, 0.0, 0.0);
        none.arrangement[0].fade_in_beats = 0.0; none.arrangement[0].fade_out_beats = 0.0;
        std::vector<float> La = render_L(with_zero), Lb = render_L(none);
        CHECK(La.size() == Lb.size() && La == Lb, "cf.explicit_zero_identical_to_absent");
    }
    // Probe D: repeats 2 → ONE ramp at the clip start (no second ramp at the repeat boundary),
    // and a single fade-out on the last repeat. extent2, fades 1/1 beat; clip_end=4 beats. Each repeat's
    // note is exactly one extent long so the two statements are adjacent (no overlap): r0 [0,2*SPB),
    // r1 [2*SPB,4*SPB). Fade-in region [0,SPB); fade-out region [3*SPB,4*SPB).
    {
        op::Project q = fade_probe_project(asset, 2.0, 2, 1.0, 1.0, 0.0, 2.0);
        std::vector<float> L = render_L(q);
        // Second repeat starts at frame at(2)=2*SPB: gain must be 1 (single ramp), not a fresh fade-in.
        bool ok_single_ramp = (L[static_cast<size_t>(2 * SPB)] == 0.25f);
        // Fade-out region is the last beat of the clip: [at(3), at(4)) = [3*SPB, 4*SPB).
        bool ok_fade_out = true;
        for (long long f = 3 * SPB; f < 4 * SPB; ++f) {
            const float g = static_cast<float>(1.0 - static_cast<double>(f - 3 * SPB) / static_cast<double>(SPB));
            if (!(L[static_cast<size_t>(f)] == 0.25f * g)) { ok_fade_out = false; break; }
        }
        CHECK(ok_single_ramp, "cf.D.one_ramp_across_repeats");
        CHECK(ok_fade_out, "cf.D.fade_out_on_last_repeat");
    }
    // Probe E: per-clip scoping — a faded clip and an unfaded sibling on the SAME track. The unfaded
    // clip's frames are bit-identical to a twin project where the first clip carries no fades.
    {
        op::Project faded = fade_probe_project(asset, 2.0, 1, 1.0, 0.0);
        op::Clip sib; sib.pattern = 0; sib.start_bar = 4.0; sib.repeats = 1;  // no fades, bar 4 → beat 16
        faded.arrangement.push_back(sib);
        op::Project plain = faded;
        plain.arrangement[0].fade_in_beats = 0.0;  // only difference: first clip unfaded
        std::vector<float> Lf = render_L(faded), Lp = render_L(plain);
        // Sibling audio lives at frames [at(16), ...) = [16*SPB, ...). It must be untouched by the fade.
        const long long sib_lo = 16 * SPB + 50;
        bool ok_sibling = true;
        for (long long f = sib_lo; f < sib_lo + 200; ++f)
            if (!(Lf[static_cast<size_t>(f)] == Lp[static_cast<size_t>(f)])) { ok_sibling = false; break; }
        bool faded_differs = !(Lf[100] == Lp[100]);  // first clip's fade-in region does differ
        CHECK(ok_sibling, "cf.E.sibling_clip_unaffected");
        CHECK(faded_differs, "cf.E.faded_clip_differs");
    }
    // Probe F: tempo map moves the region bounds via TempoMap::at. tempo [{0,120},{2,60}]: beats<8 at
    // spb 24000 (start_sample 0), beats>=8 at spb 48000 (start_sample 192000). fade_in 10 beats → N=288000.
    {
        op::Project q = fade_probe_project(asset, 4.0, 1, 10.0, 0.0);
        op::TempoPoint t0; t0.bar = 0.0; t0.bpm = 120.0;
        op::TempoPoint t1; t1.bar = 2.0; t1.bpm = 60.0;   // bar 2 → beat 8 onward at 60 bpm (spb 48000)
        q.tempo_map = {t0, t1};
        std::vector<float> L = render_L(q);
        auto at_map = [](double beats) -> long long {
            if (beats < 8.0) return static_cast<long long>(std::llround(beats * 24000.0));
            return 192000LL + static_cast<long long>(std::llround((beats - 8.0) * 48000.0));
        };
        const long long A = at_map(0.0), N = at_map(10.0) - A;   // == 288000 (a constant-120 map gives 240000)
        bool ok = true;
        for (long long f = 50000; f < 200000; f += 37000) {       // frames inside the fade-in region
            const float g = static_cast<float>(static_cast<double>(f - A) / static_cast<double>(N));
            if (!(L[static_cast<size_t>(f)] == 0.25f * g)) { ok = false; break; }
        }
        CHECK(N == 288000LL, "cf.F.tempo_map_bounds_widen_N");
        CHECK(ok, "cf.F.fade_in_bounds_follow_tempo_map");
    }

    // --- Op set_clip_fades: sets both / null clears one side / zeros restore never-faded bytes.
    using nlohmann::json;
    {
        op::Project q = fade_probe_project(asset, 2.0, 1, 0.0, 0.0);
        const std::string never_faded = saved_text(q);
        CHECK(op::apply_op(q, json{{"op", "set_clip_fades"}, {"index", 0},
                                   {"fade_in_beats", 1.5}, {"fade_out_beats", 0.25}}, err) &&
                  q.arrangement[0].fade_in_beats == 1.5 && q.arrangement[0].fade_out_beats == 0.25,
              "cf.op.sets_both");
        // null clears one side (fade-out → default 0), fade-in kept:
        CHECK(op::apply_op(q, json{{"op", "set_clip_fades"}, {"index", 0},
                                   {"fade_in_beats", 1.5}, {"fade_out_beats", nullptr}}, err) &&
                  q.arrangement[0].fade_in_beats == 1.5 && q.arrangement[0].fade_out_beats == 0.0,
              "cf.op.null_clears_one_side");
        // zeros restore the never-faded canonical bytes exactly:
        CHECK(op::apply_op(q, json{{"op", "set_clip_fades"}, {"index", 0},
                                   {"fade_in_beats", 0.0}, {"fade_out_beats", 0.0}}, err) &&
                  saved_text(q) == never_faded, "cf.op.zeros_restore_never_faded_bytes");
    }

    // --- Op rejections: exact message + project left unchanged (rejected_unchanged).
    {
        op::Project q = fade_probe_project(asset, 2.0, 1, 0.0, 0.0);
        CHECK(rejected_unchanged(q, json{{"op", "set_clip_fades"}, {"index", 0}, {"fade_in_beats", 1.0}}, err) &&
                  err == "set_clip_fades: missing argument 'fade_out_beats'", "cf.op.reject_missing_fade_out");
        CHECK(rejected_unchanged(q, json{{"op", "set_clip_fades"}, {"index", 0}, {"fade_in_beats", "x"},
                                         {"fade_out_beats", 1.0}}, err) &&
                  err == "set_clip_fades: 'fade_in_beats' must be a number or null", "cf.op.reject_non_number");
        CHECK(rejected_unchanged(q, json{{"op", "set_clip_fades"}, {"index", 0}, {"fade_in_beats", 5000.0},
                                         {"fade_out_beats", 1.0}}, err) &&
                  err == "set_clip_fades: clip.fade_in_beats must be a number in [0, 4096]", "cf.op.reject_range_via_parse");
        CHECK(rejected_unchanged(q, json{{"op", "set_clip_fades"}, {"index", 7}, {"fade_in_beats", 1.0},
                                         {"fade_out_beats", 1.0}}, err) &&
                  err == "set_clip_fades: 'index' out of range", "cf.op.reject_index_oob");
    }

    // --- History undo/redo byte-exact around a set_clip_fades op.
    {
        op::Project q = fade_probe_project(asset, 2.0, 1, 0.0, 0.0);
        const std::string before = saved_text(q);
        op::History h;
        CHECK(h.apply(q, json{{"op", "set_clip_fades"}, {"index", 0}, {"fade_in_beats", 2.5},
                              {"fade_out_beats", 1.0}}, err) && saved_text(q) != before,
              "cf.hist.applied");
        CHECK(h.undo(q) && saved_text(q) == before, "cf.hist.undo_byte_exact");
        CHECK(h.redo(q) && q.arrangement[0].fade_in_beats == 2.5 && q.arrangement[0].fade_out_beats == 1.0,
              "cf.hist.redo_restores");
    }

    // --- opd diff: one set_clip_fades (both values), after set_clip_gain, empty when equal; replay = b.
    {
        op::Project a = fade_probe_project(asset, 2.0, 1, 0.0, 0.0);
        std::vector<json> ops;
        // fades-only difference:
        op::Project b = a; b.arrangement[0].fade_in_beats = 1.5; b.arrangement[0].fade_out_beats = 0.25;
        CHECK(op::diff_projects(a, b, ops, err) &&
                  journal_text(ops) == "{\"fade_in_beats\":1.5,\"fade_out_beats\":0.25,\"index\":0,\"op\":\"set_clip_fades\"}\n",
              "cf.diff.fades_only_one_op");
        std::string apply_err;
        CHECK(op::apply_journal(a, journal_text(ops), apply_err) && saved_text(a) == saved_text(b),
              "cf.diff.replay_reproduces_b");
        // equal fades → no set_clip_fades emitted:
        op::Project c = a;  // both never-faded
        std::vector<json> ops2;
        CHECK(op::diff_projects(a, c, ops2, err) &&
                  journal_text(ops2).find("set_clip_fades") == std::string::npos, "cf.diff.equal_no_op");
        // gain + fades both differ → set_clip_gain before set_clip_fades:
        op::Project d = a; d.arrangement[0].gain_db = -6.0; d.arrangement[0].fade_in_beats = 1.0;
        std::vector<json> ops3;
        CHECK(op::diff_projects(a, d, ops3, err), "cf.diff.gain_and_fades_ok");
        const std::string j3 = journal_text(ops3);
        const size_t gp = j3.find("set_clip_gain"), fp = j3.find("set_clip_fades");
        CHECK(gp != std::string::npos && fp != std::string::npos && gp < fp, "cf.diff.gain_before_fades");
    }
}

// WP-0180: song-structure ops insert_bars / delete_bars (docs/spec/operations.md
// §Song-structure operations). Clips and the loop region move with the cut, nothing else does;
// every rejection leaves the project byte-for-byte unchanged; `opd diff` still reproduces the
// result using existing operations only.
static void test_insert_delete_bars() {
    using nlohmann::json;
    std::string err;

    // Base project = the work-package example: 8 bars of 4/4, patterns p0/p1/p2 with extents
    // 1 / 2 / 1.5 bars (explicit length_beats 4 / 8 / 6), six clips and a loop over [1, 5).
    const std::string kBody =
        R"("beats_per_bar":4,"tracks":[)"
        R"({"name":"Drums","voice":{"kind":"metronome"}},)"
        R"({"name":"Bass","voice":{"kind":"sine","freq_hz":110.0}},)"
        R"({"name":"Lead","voice":{"kind":"sine","freq_hz":440.0}}],)"
        R"("patterns":[)"
        R"({"track":0,"length_beats":4.0,"notes":[{"t":0.0,"dur":0.25}]},)"
        R"({"track":1,"length_beats":8.0,"notes":[{"t":0.0,"dur":0.5,"note":36}]},)"
        R"({"track":2,"length_beats":6.0,"notes":[{"t":0.0,"dur":0.5,"note":60}]}])";
    const std::string kClips =
        R"(,"arrangement":[)"
        R"({"pattern":0,"start_bar":0.0,"repeats":1},)"
        R"({"pattern":0,"start_bar":2.0,"repeats":4},)"
        R"({"pattern":1,"start_bar":3.0,"repeats":2},)"
        R"({"pattern":2,"start_bar":1.0,"repeats":3,"gain_db":-3.0},)"
        R"({"pattern":0,"start_bar":5.0,"repeats":1},)"
        R"({"pattern":0,"start_bar":6.5,"repeats":1}])";
    const std::string kLoop = R"(,"loop":{"start_bar":1.0,"end_bar":5.0})";
    auto text_of = [&](const std::string& length_bars, const std::string& arrangement,
                       const std::string& loop) {
        return std::string(R"({"format_version":0,"bpm":120,"sample_rate":48000,"seed":42,)"
                           R"("length_bars":)") +
               length_bars + "," + kBody + arrangement + loop + "}";
    };
    const std::string base = text_of("8", kClips, kLoop);

    auto starts_of = [](const op::Project& p) {
        std::vector<double> v;
        for (const op::Clip& c : p.arrangement) v.push_back(c.start_bar);
        return v;
    };
    auto repeats_of = [](const op::Project& p) {
        std::vector<int> v;
        for (const op::Clip& c : p.arrangement) v.push_back(c.repeats);
        return v;
    };
    // "pattern@start_bar xrepeats/gain_db;" per clip, in arrangement order — readable on failure.
    auto clips_of = [](const op::Project& p) {
        std::string s;
        char buf[96];
        for (const op::Clip& c : p.arrangement) {
            std::snprintf(buf, sizeof buf, "%d@%g x%d/%+.4g;", c.pattern, c.start_bar, c.repeats,
                          c.gain_db);
            s += buf;
        }
        return s;
    };
    auto rejected_unchanged = [&](op::Project& p, const json& o, const std::string& want) {
        const std::string before = op::project_bytes(p);
        std::string e;
        const bool ok = op::apply_op(p, o, e);
        return !ok && e == want && op::project_bytes(p) == before;
    };

    // (1) The base project parses as described: 8 bars, 6 clips, loop [1, 5).
    op::Project b;
    CHECK(parse_text(base, b, err) && b.length_bars == 8.0 && b.arrangement.size() == 6 &&
              b.loop && b.loop->start_bar == 1.0 && b.loop->end_bar == 5.0,
          "sb.base_parses");

    // (2) insert_bars in the middle: later clips move, earlier ones stay put, the loop grows.
    op::Project q;
    CHECK(parse_text(base, q, err) &&
              op::apply_op(q, json{{"op", "insert_bars"}, {"at_bar", 3}, {"count", 2}}, err),
          "sb.insert_mid_applies");
    CHECK(q.length_bars == 10.0, "sb.insert_mid_length_grows_by_count");
    CHECK(starts_of(q) == std::vector<double>({0.0, 2.0, 5.0, 1.0, 7.0, 8.5}),
          "sb.insert_mid_starts_shifted");
    CHECK(repeats_of(q) == std::vector<int>({1, 4, 2, 3, 1, 1}) && q.arrangement[3].gain_db == -3.0,
          "sb.insert_mid_repeats_and_gain_untouched");
    CHECK(q.loop && q.loop->start_bar == 1.0 && q.loop->end_bar == 7.0, "sb.insert_mid_loop_grows");
    {  // the same project written out by hand parses to identical saved bytes
        const std::string want = text_of(
            "10",
            R"(,"arrangement":[)"
            R"({"pattern":0,"start_bar":0.0,"repeats":1},)"
            R"({"pattern":0,"start_bar":2.0,"repeats":4},)"
            R"({"pattern":1,"start_bar":5.0,"repeats":2},)"
            R"({"pattern":2,"start_bar":1.0,"repeats":3,"gain_db":-3.0},)"
            R"({"pattern":0,"start_bar":7.0,"repeats":1},)"
            R"({"pattern":0,"start_bar":8.5,"repeats":1}])",
            R"(,"loop":{"start_bar":1.0,"end_bar":7.0})");
        op::Project w;
        CHECK(parse_text(want, w, err) && saved_text(q) == saved_text(w),
              "sb.insert_mid_bytes_match_handbuilt");
    }

    // (3) insert_bars at bar 0: every clip moves, the whole loop moves.
    op::Project z;
    CHECK(parse_text(base, z, err) &&
              op::apply_op(z, json{{"op", "insert_bars"}, {"at_bar", 0}, {"count", 2}}, err) &&
              z.length_bars == 10.0 &&
              starts_of(z) == std::vector<double>({2.0, 4.0, 5.0, 3.0, 7.0, 8.5}),
          "sb.insert_at_zero_shifts_every_clip");
    CHECK(z.loop && z.loop->start_bar == 3.0 && z.loop->end_bar == 7.0,
          "sb.insert_at_zero_moves_whole_loop");

    // (4) insert_bars at length_bars appends: no clip and no loop bound moves.
    op::Project ap;
    CHECK(parse_text(base, ap, err) &&
              op::apply_op(ap, json{{"op", "insert_bars"}, {"at_bar", 8}, {"count", 2}}, err) &&
              ap.length_bars == 10.0 && ap.arrangement.size() == 6 &&
              starts_of(ap) == std::vector<double>({0.0, 2.0, 3.0, 1.0, 5.0, 6.5}),
          "sb.insert_at_length_appends");
    CHECK(ap.loop && ap.loop->start_bar == 1.0 && ap.loop->end_bar == 5.0,
          "sb.insert_at_length_loop_unchanged");

    // (5) insert_bars at the loop start moves both bounds; at the loop end the loop stays put
    //     (the cut opens after it) while a clip sitting exactly on the boundary still moves.
    op::Project l1, l2;
    CHECK(parse_text(base, l1, err) &&
              op::apply_op(l1, json{{"op", "insert_bars"}, {"at_bar", 1}, {"count", 2}}, err) &&
              l1.loop && l1.loop->start_bar == 3.0 && l1.loop->end_bar == 7.0 &&
              starts_of(l1) == std::vector<double>({0.0, 4.0, 5.0, 3.0, 7.0, 8.5}),
          "sb.insert_at_loop_start_moves_both_bounds");
    CHECK(parse_text(base, l2, err) &&
              op::apply_op(l2, json{{"op", "insert_bars"}, {"at_bar", 5}, {"count", 2}}, err) &&
              l2.loop && l2.loop->start_bar == 1.0 && l2.loop->end_bar == 5.0 &&
              starts_of(l2) == std::vector<double>({0.0, 2.0, 3.0, 1.0, 7.0, 8.5}),
          "sb.insert_at_loop_end_loop_unchanged_boundary_clip_moves");

    // (6) count 4096 is the accepted upper bound.
    op::Project big;
    CHECK(parse_text(base, big, err) &&
              op::apply_op(big, json{{"op", "insert_bars"}, {"at_bar", 0}, {"count", 4096}}, err) &&
              big.length_bars == 8.0 + 4096.0 && starts_of(big).front() == 4096.0,
          "sb.insert_count_4096_accepted");

    // (7) delete_bars of the work-package example: clip removed, repeats trimmed, later clips
    //     shift earlier, gain kept, loop end pulled back to the cut.
    op::Project d;
    CHECK(parse_text(base, d, err) &&
              op::apply_op(d, json{{"op", "delete_bars"}, {"at_bar", 3}, {"count", 2}}, err),
          "sb.delete_example_applies");
    CHECK(d.length_bars == 6.0, "sb.delete_length_shrinks_by_count");
    CHECK(clips_of(d) == "0@0 x1/+0;0@2 x1/+0;2@1 x2/-3;0@3 x1/+0;0@4.5 x1/+0;",
          "sb.delete_clips_removed_trimmed_shifted");
    CHECK(d.loop && d.loop->start_bar == 1.0 && d.loop->end_bar == 3.0,
          "sb.delete_loop_end_shifted_earlier");
    {
        const std::string want = text_of(
            "6",
            R"(,"arrangement":[)"
            R"({"pattern":0,"start_bar":0.0,"repeats":1},)"
            R"({"pattern":0,"start_bar":2.0,"repeats":1},)"
            R"({"pattern":2,"start_bar":1.0,"repeats":2,"gain_db":-3.0},)"
            R"({"pattern":0,"start_bar":3.0,"repeats":1},)"
            R"({"pattern":0,"start_bar":4.5,"repeats":1}])",
            R"(,"loop":{"start_bar":1.0,"end_bar":3.0})");
        op::Project w;
        CHECK(parse_text(want, w, err) && saved_text(d) == saved_text(w),
              "sb.delete_bytes_match_handbuilt");
    }

    // (8) delete_bars that only cuts a gap: clips before keep their repeats, the clip starting in
    //     the cut is dropped, later ones move earlier by count.
    op::Project g;
    CHECK(parse_text(base, g, err) &&
              op::apply_op(g, json{{"op", "delete_bars"}, {"at_bar", 1}, {"count", 1}}, err) &&
              g.length_bars == 7.0 &&
              clips_of(g) == "0@0 x1/+0;0@1 x4/+0;1@2 x2/+0;0@4 x1/+0;0@5.5 x1/+0;",
          "sb.delete_shifts_later_clips_earlier");

    // (9) Loop region mapping on a clip-free project (length 8): the start is clamped to `a`, an
    //     entirely swallowed loop is removed, a later loop moves earlier, an earlier one stays.
    op::Project lc;
    CHECK(parse_text(text_of("8", "", R"(,"loop":{"start_bar":2.0,"end_bar":6.0})"), lc, err) &&
              op::apply_op(lc, json{{"op", "delete_bars"}, {"at_bar", 1}, {"count", 2}}, err) &&
              lc.loop && lc.loop->start_bar == 1.0 && lc.loop->end_bar == 4.0,
          "sb.delete_loop_start_clamped_to_cut");
    op::Project lr;
    CHECK(parse_text(text_of("8", "", R"(,"loop":{"start_bar":2.0,"end_bar":3.0})"), lr, err) &&
              op::apply_op(lr, json{{"op", "delete_bars"}, {"at_bar", 2}, {"count", 1}}, err) &&
              !lr.loop,
          "sb.delete_loop_swallowed_is_removed");
    op::Project lm;
    CHECK(parse_text(text_of("8", "", R"(,"loop":{"start_bar":5.0,"end_bar":7.0})"), lm, err) &&
              op::apply_op(lm, json{{"op", "delete_bars"}, {"at_bar", 1}, {"count", 2}}, err) &&
              lm.loop && lm.loop->start_bar == 3.0 && lm.loop->end_bar == 5.0,
          "sb.delete_loop_after_cut_shifts");
    op::Project lu;
    CHECK(parse_text(text_of("8", "", R"(,"loop":{"start_bar":1.0,"end_bar":2.0})"), lu, err) &&
              op::apply_op(lu, json{{"op", "delete_bars"}, {"at_bar", 3}, {"count", 2}}, err) &&
              lu.loop && lu.loop->start_bar == 1.0 && lu.loop->end_bar == 2.0,
          "sb.delete_loop_before_cut_unchanged");

    // (10) A kept statement that starts before the cut is never shortened: a 2-bar clip at bar 2
    //      survives delete(3,1) whole, and its pattern notes are untouched.
    op::Project st;
    CHECK(parse_text(text_of("8", R"(,"arrangement":[{"pattern":1,"start_bar":2.0,"repeats":1}])", ""),
                     st, err) &&
              op::apply_op(st, json{{"op", "delete_bars"}, {"at_bar", 3}, {"count", 1}}, err) &&
              st.length_bars == 7.0 && clips_of(st) == "1@2 x1/+0;" &&
              st.patterns[1].length_beats && *st.patterns[1].length_beats == 8.0 &&
              st.patterns[1].notes.size() == 1,
          "sb.delete_straddling_statement_kept_not_trimmed");

    // (10b) Statement spacing comes from pattern_extent: derived from the notes when a pattern has
    //       no explicit length_beats, and an empty pattern (extent 0) keeps all of its repeats.
    const std::string derived =
        std::string(R"({"format_version":0,"bpm":120,"sample_rate":48000,"seed":42,"length_bars":8,)"
                    R"("beats_per_bar":4,"tracks":[{"name":"T","voice":{"kind":"sine"}}],)"
                    R"("patterns":[)"
                    R"({"track":0,"notes":[{"t":0.0,"dur":2.0}]},)"
                    R"({"track":0,"notes":[]}],)"
                    R"("arrangement":[)"
                    R"({"pattern":0,"start_bar":1.0,"repeats":4},)"
                    R"json({"pattern":1,"start_bar":0.5,"repeats":3}]})json");
    op::Project dv;
    CHECK(parse_text(derived, dv, err) && op::pattern_extent(dv.patterns[0]) == 2.0 &&
              op::pattern_extent(dv.patterns[1]) == 0.0,
          "sb.derived_extents_are_as_expected");
    op::Project dt;
    CHECK(parse_text(derived, dt, err) &&
              op::apply_op(dt, json{{"op", "delete_bars"}, {"at_bar", 2}, {"count", 1}}, err) &&
              dt.length_bars == 7.0 && clips_of(dt) == "0@1 x2/+0;1@0.5 x3/+0;",
          "sb.delete_trims_by_derived_extent_empty_pattern_keeps_all");

    // (11) Legacy project without an arrangement: insert keeps the key absent, delete shortens.
    const std::string legacy = text_of("4", "", "");
    op::Project lg;
    CHECK(parse_text(legacy, lg, err) && lg.arrangement.empty() &&
              op::apply_op(lg, json{{"op", "insert_bars"}, {"at_bar", 2}, {"count", 3}}, err) &&
              lg.length_bars == 7.0 && lg.arrangement.empty() &&
              saved_text(lg).find("arrangement") == std::string::npos,
          "sb.legacy_insert_keeps_arrangement_absent");
    op::Project ld;
    CHECK(parse_text(legacy, ld, err) &&
              op::apply_op(ld, json{{"op", "delete_bars"}, {"at_bar", 1}, {"count", 3}}, err) &&
              ld.length_bars == 1.0 && saved_text(ld).find("arrangement") == std::string::npos,
          "sb.legacy_delete_shortens_length");

    // (12) Argument checks in spec order; every rejection leaves the project bytes untouched.
    op::Project r;
    CHECK(parse_text(base, r, err), "sb.rules_base_parse");
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", 1}, {"count", 2}, {"oops", 1}},
                             "insert_bars: unknown argument 'oops'"),
          "sb.err_unknown_argument_insert");
    CHECK(rejected_unchanged(r, json{{"op", "delete_bars"}, {"at_bar", 1}, {"count", 2}, {"oops", 1}},
                             "delete_bars: unknown argument 'oops'"),
          "sb.err_unknown_argument_delete");
    // Unknown key is checked before the arguments are present at all.
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"oops", 1}},
                             "insert_bars: unknown argument 'oops'"),
          "sb.err_unknown_key_before_missing_argument");
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"count", 2}},
                             "insert_bars: missing argument 'at_bar'"),
          "sb.err_missing_at_bar");
    {  // `1.0` is a JSON float: rejected even though integral
        const json o = json::parse(R"({"op":"insert_bars","at_bar":1.0,"count":2})");
        CHECK(rejected_unchanged(r, o, "insert_bars: 'at_bar' must be an integer"),
              "sb.err_at_bar_float_rejected");
    }
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", 1}},
                             "insert_bars: missing argument 'count'"),
          "sb.err_missing_count");
    {
        const json o = json::parse(R"({"op":"delete_bars","at_bar":1,"count":2.5})");
        CHECK(rejected_unchanged(r, o, "delete_bars: 'count' must be an integer"),
              "sb.err_count_not_integer");
    }
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", -1}, {"count", 2}},
                             "insert_bars: 'at_bar' out of range"),
          "sb.err_at_bar_negative");
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", 1000000001}, {"count", 2}},
                             "insert_bars: 'at_bar' out of range"),
          "sb.err_at_bar_above_max");
    {  // count == 0 arrives as an UNSIGNED JSON value: the lower bound must still be enforced
        const json oi = json::parse(R"({"op":"insert_bars","at_bar":1,"count":0})");
        const json od = json::parse(R"({"op":"delete_bars","at_bar":1,"count":0})");
        CHECK(rejected_unchanged(r, oi, "insert_bars: 'count' out of range [1..4096]"),
              "sb.err_count_zero_insert_rejected");
        CHECK(rejected_unchanged(r, od, "delete_bars: 'count' out of range [1..4096]"),
              "sb.err_count_zero_delete_rejected");
    }
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", 1}, {"count", -2}},
                             "insert_bars: 'count' out of range [1..4096]"),
          "sb.err_count_negative");
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", 1}, {"count", 4097}},
                             "insert_bars: 'count' out of range [1..4096]"),
          "sb.err_count_above_max");
    // count is range-checked before the project-dependent past-the-end check (99 <= 1e9 passes).
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", 99}, {"count", 0}},
                             "insert_bars: 'count' out of range [1..4096]"),
          "sb.err_count_range_before_past_end_check");
    CHECK(rejected_unchanged(r, json{{"op", "insert_bars"}, {"at_bar", 9}, {"count", 1}},
                             "insert_bars: 'at_bar' out of range"),
          "sb.err_insert_past_end");
    CHECK(rejected_unchanged(r, json{{"op", "delete_bars"}, {"at_bar", 7}, {"count", 2}},
                             "delete_bars: range exceeds length_bars"),
          "sb.err_delete_range_exceeds_length");
    CHECK(rejected_unchanged(r, json{{"op", "delete_bars"}, {"at_bar", 0}, {"count", 8}},
                             "delete_bars: would remove every clip"),
          "sb.err_would_remove_every_clip");

    // (13) A non-empty tempo map is refused before any project-dependent range check.
    op::Project tm;
    CHECK(parse_text(text_of("8", kClips, R"(,"tempo_map":[{"bar":0.0,"bpm":120.0},{"bar":4.0,"bpm":90.0}])"),
                     tm, err),
          "sb.tempo_base_parse");
    CHECK(rejected_unchanged(tm, json{{"op", "insert_bars"}, {"at_bar", 2}, {"count", 1}},
                             "insert_bars: tempo_map is not supported"),
          "sb.err_tempo_map_insert");
    CHECK(rejected_unchanged(tm, json{{"op", "delete_bars"}, {"at_bar", 2}, {"count", 1}},
                             "delete_bars: tempo_map is not supported"),
          "sb.err_tempo_map_delete");
    CHECK(rejected_unchanged(tm, json{{"op", "insert_bars"}, {"at_bar", 99}, {"count", 1}},
                             "insert_bars: tempo_map is not supported"),
          "sb.err_tempo_map_before_past_end_check");

    // (14) Deleting a clip-free project down to zero bars fails in the model parse.
    op::Project lz;
    CHECK(parse_text(legacy, lz, err) &&
              rejected_unchanged(lz, json{{"op", "delete_bars"}, {"at_bar", 0}, {"count", 4}},
                                 "delete_bars: length_bars must be > 0"),
          "sb.err_delete_to_zero_length");

    // (15) History: undo and redo restore the exact bytes; a rejected op changes nothing at all.
    op::Project h;
    CHECK(parse_text(base, h, err), "sb.history_base_parse");
    const std::string bytes0 = saved_text(h);
    op::History hist;
    CHECK(hist.apply(h, json{{"op", "insert_bars"}, {"at_bar", 3}, {"count", 2}}, err) &&
              hist.undo_depth() == 1 && saved_text(h) != bytes0,
          "sb.history_insert_pushes_one_snapshot");
    const std::string bytes1 = saved_text(h);
    CHECK(hist.undo(h) && saved_text(h) == bytes0, "sb.history_undo_byte_exact");
    CHECK(hist.redo(h) && saved_text(h) == bytes1, "sb.history_redo_byte_exact");
    // (the project is the 10-bar one from the redo, so bars [9, 11) run past its end)
    CHECK(!hist.apply(h, json{{"op", "delete_bars"}, {"at_bar", 9}, {"count", 2}}, err) &&
              hist.undo_depth() == 1 && saved_text(h) == bytes1,
          "sb.history_rejected_leaves_depth_and_bytes");
    CHECK(hist.apply(h, json{{"op", "delete_bars"}, {"at_bar", 3}, {"count", 2}}, err) &&
              hist.undo_depth() == 2 && hist.undo(h) && saved_text(h) == bytes1,
          "sb.history_delete_undo_byte_exact");

    // (16) opd diff never emits these ops, but its journal reproduces the result byte-for-byte.
    op::Project after_ins;
    CHECK(parse_text(base, after_ins, err) &&
              op::apply_op(after_ins, json{{"op", "insert_bars"}, {"at_bar", 3}, {"count", 2}}, err),
          "sb.diff_insert_base_ready");
    std::vector<json> ops;
    CHECK(op::diff_projects(b, after_ins, ops, err) && !ops.empty(), "sb.diff_insert_succeeds");
    const std::string journal = journal_text(ops);
    CHECK(journal.find("insert_bars") == std::string::npos &&
              journal.find("delete_bars") == std::string::npos,
          "sb.diff_emits_no_song_structure_ops");
    op::Project replay = b;
    CHECK(op::apply_journal(replay, journal, err) && saved_text(replay) == saved_text(after_ins),
          "sb.diff_insert_replay_byte_exact");

    op::Project after_del;
    CHECK(parse_text(base, after_del, err) &&
              op::apply_op(after_del, json{{"op", "delete_bars"}, {"at_bar", 3}, {"count", 2}}, err),
          "sb.diff_delete_base_ready");
    std::vector<json> ops2;
    CHECK(op::diff_projects(b, after_del, ops2, err) && !ops2.empty(), "sb.diff_delete_succeeds");
    const std::string journal2 = journal_text(ops2);
    CHECK(journal2.find("insert_bars") == std::string::npos &&
              journal2.find("delete_bars") == std::string::npos,
          "sb.diff_delete_emits_no_song_structure_ops");
    op::Project replay2 = b;
    CHECK(op::apply_journal(replay2, journal2, err) && saved_text(replay2) == saved_text(after_del),
          "sb.diff_delete_replay_byte_exact");
}

int main() {
    test_pcm16();
    test_rng();
    test_dsp();
    test_model();
    test_wav_roundtrip();
    test_wav_formats();
    test_render();
    test_arrangement();
    test_synth();
    test_midi();
    test_tempo_map();
    test_render_range();
    test_click();
    test_bundle();
    test_velocity();
    test_midi_merge();
    test_tempo_midi();
    test_attachments();
    test_clip_gain();
    test_clip_fades();
    test_mixer();
    test_json_fuzz();
    test_wav_fuzz();
    test_aiff();
    test_bundle_fuzz();
    test_arrangement_export();
    test_ops();
    test_ops_property();
    test_document();
    test_document_autosave();
    test_resample();
    test_project_bytes();
    test_analyze();
    test_opd_diff();
    test_resample_polyphase();
    test_step_sequencer();
    test_loop_region();
    test_sampler_trim();
    test_synth_v1();
    test_pattern_length();
    test_new_project();
    test_pattern_swing();
    test_insert_delete_bars();
    test_sampler_repitch();
    test_sample_index();
    test_duplicate_pattern();
    test_player();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    if (g_failures == 0) {
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir(), ec);
    } else {
        std::printf("kept scratch dir %s\n", tmp_dir().c_str());
    }
    return g_failures;
}
