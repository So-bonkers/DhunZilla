// Unit tests for the deterministic core. Boring assert-based runner — no framework deps.
// Exit code = number of failures. Run: ./build/op_tests
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/bundle.hpp"
#include "engine/dsp.hpp"
#include "engine/midi.hpp"
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

    write_text("/tmp/op_test_syn_note.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"synth"}}],"patterns":[{"track":0,"notes":[{"t":0,"dur":1,"note":128}]}]})");
    op::Project pb;
    CHECK(!op::load_project("/tmp/op_test_syn_note.opj", pb, err), "model.rejects_midi_note_128");

    op::Project ps = p;
    CHECK(op::save_project("/tmp/op_test_synth_rt.opj", ps, err), "synth.save_ok");
    op::Project pl;
    CHECK(op::load_project("/tmp/op_test_synth_rt.opj", pl, err) &&
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
        std::string tmp = "/tmp/op_fuzz.mid";
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
        { std::ofstream w("/tmp/op_mut.mid", std::ios::binary); w.write(reinterpret_cast<const char*>(m.data()), static_cast<std::streamsize>(m.size())); }
        op::Project mp;
        if (op::load_midi("/tmp/op_mut.mid", mp, err)) {
            ++mutants_parsed;
            for (const auto& pat : mp.patterns)
                for (const auto& nn : pat.notes)
                    if (!(nn.dur_beats > 0.0) || nn.midi_note < 0 || nn.midi_note > 127 || !(mp.bpm > 0.0)) mut_ok = false;
        }
    }
    CHECK(mut_ok, "midi.mutation_fuzz_no_crash_invariants_hold");
    std::printf("INFO midi mutation fuzz: %zu/500 mutants parsed\n", mutants_parsed);

    // wrong magic + SMPTE division rejected with actionable errors
    write_text("/tmp/op_badmagic.mid", "RIFFjunkjunk");
    op::Project bp;
    CHECK(!op::load_midi("/tmp/op_badmagic.mid", bp, err), "midi.rejects_bad_magic");

    // --- export (WP-0104): byte-deterministic + exact content round-trip
    op::Project src;
    CHECK(op::load_midi(fix, src, err), "midi.rt_source_load");
    CHECK(op::save_midi("/tmp/op_rt_a.mid", src, err), "midi.export_ok");
    auto slurp = [](const char* p) {
        std::ifstream f(p, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    CHECK(op::save_midi("/tmp/op_rt_b.mid", src, err), "midi.export2_ok");
    CHECK(slurp("/tmp/op_rt_a.mid") == slurp("/tmp/op_rt_b.mid"), "midi.export_byte_deterministic");
    op::Project back;
    CHECK(op::load_midi("/tmp/op_rt_a.mid", back, err), "midi.reimport_ok");
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
    CHECK(op::save_midi("/tmp/op_legacy.mid", lp, err), "midi.export_legacy_ok");
    {
        size_t n_notes = 0;
        op::Project lb;
        if (op::load_midi("/tmp/op_legacy.mid", lb, err))
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

    write_text("/tmp/op_tm_badbpm.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[],"tempo_map":[{"bar":0,"bpm":120},{"bar":2,"bpm":-5}]})");
    op::Project pb;
    CHECK(!op::load_project("/tmp/op_tm_badbpm.opj", pb, err), "model.rejects_bad_tempo_bpm");
    write_text("/tmp/op_tm_nozero.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[],"tempo_map":[{"bar":1,"bpm":120}]})");
    CHECK(!op::load_project("/tmp/op_tm_nozero.opj", pb, err), "model.rejects_tempo_map_not_starting_at_zero");

    CHECK(op::save_project("/tmp/op_tm_rt.opj", c, err) && op::load_project("/tmp/op_tm_rt.opj", pb, err) &&
          pb.tempo_map.size() == 2 && pb.tempo_map[1].bpm == 60.0, "tempo.save_roundtrip");
}

// --- .opd bundles (WP-0110): deterministic pack, verified unpack, render equivalence, guards.
static void test_bundle() {
    std::string err;
    // Source project with a sampler asset so the bundle actually has to carry files.
    std::error_code sec;
    std::filesystem::remove_all("/tmp/op_bd", sec);
    std::filesystem::create_directories("/tmp/op_bd/kits", sec);
    const std::string dir = "/tmp/op_bd";
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
    write_text("/tmp/op_vel.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1,"vel":0.5},{"t":2,"dur":1}]}]})");
    op::Project p;
    CHECK(op::load_project("/tmp/op_vel.opj", p, err) && p.patterns[0].notes.size() == 2 &&
          p.patterns[0].notes[0].velocity == 0.5 && p.patterns[0].notes[1].velocity == 1.0,
          "vel.parse_default_and_explicit");
    write_text("/tmp/op_vel0.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1,"vel":0.0}]}]})");
    op::Project pb;
    CHECK(!op::load_project("/tmp/op_vel0.opj", pb, err), "model.rejects_vel_zero");
    write_text("/tmp/op_vel2.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1,"vel":1.5}]}]})");
    CHECK(!op::load_project("/tmp/op_vel2.opj", pb, err), "model.rejects_vel_above_one");

    // Save: velocity omitted when default → legacy canonical bytes unchanged.
    op::Project q;
    q.bpm = 120.0; q.length_bars = 1.0; q.sample_rate = 48000;
    op::Track t; t.name = "S"; t.voice.kind = op::VoiceKind::Sine; t.voice.freq_hz = 220.0f;
    q.tracks = {t};
    op::Pattern pat; pat.track = 0; pat.notes.push_back({0.0, 1.0, -1});
    q.patterns = {pat};
    CHECK(op::save_project("/tmp/op_vel_save.opj", q, err), "vel.save_ok");
    {
        std::ifstream f("/tmp/op_vel_save.opj");
        const std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(txt.find("\"vel\"") == std::string::npos, "vel.omitted_from_save_when_default");
    }

    // Render: vel 1.0 (no field) vs parsed project identical bit-for-bit; vel 0.5 halves peak exactly.
    op::wav::StereoBuffer b1, bh;
    CHECK(op::render_project(q, "", b1, err), "vel.render_default_ok");
    CHECK(op::load_project("/tmp/op_vel.opj", p, err) && p.sample_rate == 48000, "vel.reload_ok");
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
    CHECK(op::save_midi("/tmp/op_vel.mid", mp, err), "vel.midi_export_ok");
    op::Project mb;
    CHECK(op::load_midi("/tmp/op_vel.mid", mb, err), "vel.midi_reimport_ok");
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
    CHECK(!op::import_midi_into(host, "/tmp/definitely_missing_file.mid", err), "merge.missing_fails");
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
    CHECK(op::save_midi("/tmp/op_tm.mid", p, err), "tmid.export_ok");
    std::ifstream f("/tmp/op_tm.mid", std::ios::binary);
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
    CHECK(op::load_midi("/tmp/op_tm.mid", q, err), "tmid.reimport_ok");
    CHECK(q.tempo_map.size() == 2 && q.tempo_map[1].bar == 4.0 &&
          std::fabs(q.tempo_map[1].bpm - 72.0) < 1e-3, "tmid.map_restored");

    // Single-tempo byte stability: empty map and {0,bpm} export identically.
    op::Project e = p; e.tempo_map.clear();
    op::Project s = p; s.tempo_map = {{0.0, 120.0}};
    CHECK(op::save_midi("/tmp/op_tm_empty.mid", e, err) && op::save_midi("/tmp/op_tm_single.mid", s, err), "tmid.singles_save");
    std::ifstream f1("/tmp/op_tm_empty.mid", std::ios::binary), f2("/tmp/op_tm_single.mid", std::ios::binary);
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
    write_text("/tmp/op_att.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[],"attachments":[{"path":"stems/drums.wav","kind":"stem","producer":"demucs"},
                                      {"path":"hum.mid","kind":"transcription"}]})");
    op::Project p;
    CHECK(op::load_project("/tmp/op_att.opj", p, err) && p.attachments.size() == 2 &&
          p.attachments[0].producer == "demucs" && p.attachments[1].producer.empty(),
          "att.parse_two");
    const char* bad[] = {
        R"([{"path":"/abs/x.wav","kind":"stem"}])",
        R"([{"path":"../escape.wav","kind":"stem"}])",
        R"([{"path":"ok.wav","kind":"magic"}])",
        R"([{"path":"","kind":"stem"}])"};
    for (const char* body : bad) {
        write_text("/tmp/op_att_bad.opj",
                   std::string(R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],"patterns":[],"attachments":)") +
                   body + "}");
        op::Project pb;
        CHECK(!op::load_project("/tmp/op_att_bad.opj", pb, err), "att.rejects_bad_input");
    }

    // add_attachment op: ok / dup / invalid-kind.
    op::Attachment a{"stems/bass.wav", "stem", "demucs ht"};
    CHECK(op::add_attachment(p, a, err) && p.attachments.size() == 3, "att.add_ok");
    CHECK(!op::add_attachment(p, a, err), "att.reject_duplicate");
    CHECK(!op::add_attachment(p, {"x.wav", "hologram", ""}, err), "att.reject_bad_kind");

    // Save omits attachments when empty; preserves them otherwise (round-trip stable).
    op::Project q = p;
    CHECK(op::save_project("/tmp/op_att_rt.opj", q, err), "att.save_ok");
    op::Project r;
    bool same = op::load_project("/tmp/op_att_rt.opj", r, err) && r.attachments.size() == 3 &&
                r.attachments[2].path == "stems/bass.wav" && r.attachments[2].producer == "demucs ht";
    CHECK(same, "att.roundtrip");

    // Bundle carries existing attachments (deduped with samplers); missing ones are skipped.
    std::error_code sec;
    std::filesystem::remove_all("/tmp/op_att_bd", sec);
    std::filesystem::create_directories("/tmp/op_att_bd/stems", sec);
    std::filesystem::create_directories("/tmp/op_att_bd/kits", sec);
    auto copyfile = [](const char* a, const char* b) {
        std::ifstream src(a, std::ios::binary);
        std::ofstream dst(b, std::ios::binary);
        dst << src.rdbuf();
    };
    copyfile("/tmp/op_bd/kits/kick.wav", "/tmp/op_att_bd/stems/drums.wav");  // fake stem
    copyfile("/tmp/op_bd/kits/kick.wav", "/tmp/op_att_bd/kits/kick.wav");
    op::Project bp;
    bp.bpm = 120.0; bp.length_bars = 1.0; bp.sample_rate = 48000;
    op::Track tk; tk.name = "K"; tk.voice.kind = op::VoiceKind::Sampler; tk.voice.path = "kits/kick.wav";
    bp.tracks = {tk};
    CHECK(op::add_attachment(bp, {"stems/drums.wav", "stem", "demucs"}, err), "att.bundle_add");
    CHECK(op::add_attachment(bp, {"gone/missing.wav", "other", ""}, err), "att.bundle_add_missing_ref");
    CHECK(op::save_project("/tmp/op_att_bd/song.opj", bp, err), "att.bundle_save");
    CHECK(op::pack_project("/tmp/op_att_bd/song.opj", "/tmp/op_att_bd/s.opd", err), "att.pack_ok");
    std::vector<std::string> entries;
    CHECK(op::list_bundle("/tmp/op_att_bd/s.opd", entries, err), "att.list_ok");
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
    write_text("/tmp/op_cg.opj", R"({"format_version":0,"bpm":120,"length_bars":4,
        "tracks":[{"voice":{"kind":"sine","freq_hz":220}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":3.5}]}],
        "arrangement":[{"pattern":0,"start_bar":0},{"pattern":0,"start_bar":2,"gain_db":-6.0}]})");
    op::Project p;
    CHECK(op::load_project("/tmp/op_cg.opj", p, err) && p.arrangement.size() == 2 &&
          p.arrangement[0].gain_db == 0.0 && p.arrangement[1].gain_db == -6.0, "cg.parse");
    write_text("/tmp/op_cg_bad.opj", R"({"format_version":0,"tracks":[{"voice":{"kind":"sine"}}],
        "patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}],
        "arrangement":[{"pattern":0,"gain_db":30.0}]})");
    op::Project pb;
    CHECK(!op::load_project("/tmp/op_cg_bad.opj", pb, err), "cg.rejects_out_of_range");

    // Save: omitted at unity, preserved otherwise (round-trip).
    op::Project q = p;
    CHECK(op::save_project("/tmp/op_cg_rt.opj", q, err), "cg.save_ok");
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
    CHECK(arrangement_slice("/tmp/op_cg_rt.opj").find("gain_db") != std::string::npos, "cg.saved_when_set");
    op::Project r;
    CHECK(op::load_project("/tmp/op_cg_rt.opj", r, err) && r.arrangement[1].gain_db == -6.0, "cg.roundtrip");
    op::Project unity = p;
    unity.arrangement[1].gain_db = 0.0;
    CHECK(op::save_project("/tmp/op_cg_u.opj", unity, err), "cg.save_unity_ok");
    CHECK(arrangement_slice("/tmp/op_cg_u.opj").find("gain_db") == std::string::npos, "cg.omitted_at_unity");

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
    CHECK(op::save_project("/tmp/op_fz_seed.opj", rich, err), "fz.seed_saved");
    std::ifstream f("/tmp/op_fz_seed.opj", std::ios::binary);
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
        std::ofstream w("/tmp/op_fz_t.opj", std::ios::binary);
        w.write(seed.data(), static_cast<std::streamsize>(i));
        w.close();
        const int r = try_load("/tmp/op_fz_t.opj");
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
        std::ofstream w("/tmp/op_fz_m.opj", std::ios::binary);
        w.write(m.data(), static_cast<std::streamsize>(m.size()));
        w.close();
        const int r = try_load("/tmp/op_fz_m.opj");
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
                std::ofstream w("/tmp/op_fz_tc.opj", std::ios::binary);
                w << m;
                w.close();
                ++cases;
                if (try_load("/tmp/op_fz_tc.opj") < 0) ++escaped;  // -1 escaped, -2 invariant broken
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
    CHECK(op::wav::write_pcm16_stereo("/tmp/op_wf_seed.wav", buf, err), "wf.seed_saved");

    auto patch_u16 = [](std::vector<uint8_t>& v, size_t off, uint16_t x) {
        v[off] = static_cast<uint8_t>(x & 0xFF);
        v[off + 1] = static_cast<uint8_t>(x >> 8);
    };
    auto patch_u32 = [](std::vector<uint8_t>& v, size_t off, uint32_t x) {
        for (int i = 0; i < 4; ++i) v[off + i] = static_cast<uint8_t>((x >> (8 * i)) & 0xFFu);
    };
    std::ifstream f("/tmp/op_wf_seed.wav", std::ios::binary);
    const std::vector<uint8_t> seed((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    auto load_must_survive = [&](const std::vector<uint8_t>& bytes, const char* label, bool must_reject) {
        std::ofstream w("/tmp/op_wf_case.wav", std::ios::binary);
        w.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        w.close();
        op::wav::ReadResult r;
        try {
            r = op::wav::read_wav("/tmp/op_wf_case.wav");  // must return, never abort/OOB
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

int main() {
    test_pcm16();
    test_rng();
    test_dsp();
    test_model();
    test_wav_roundtrip();
    test_render();
    test_arrangement();
    test_synth();
    test_midi();
    test_tempo_map();
    test_bundle();
    test_velocity();
    test_midi_merge();
    test_tempo_midi();
    test_attachments();
    test_clip_gain();
    test_json_fuzz();
    test_wav_fuzz();
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
