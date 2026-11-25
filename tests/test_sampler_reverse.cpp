// Unit tests for sampler reverse playback (WP-0231, TASKS/WP-0231-sampler-reverse-playback.md):
// the project JSON `reverse` key, its parse/canonical-save rules, the render semantics
// (resample → trim → reverse → repitch), the `set_sampler_reverse` op and its `opd diff` support.
// Boring assert-based runner — no framework deps. Exit code = number of failures.
// Run: ./build/op_sampler_reverse_tests
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/dsp.hpp"
#include "engine/model.hpp"
#include "engine/ops.hpp"
#include "engine/render.hpp"
#include "engine/wav.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace {

using nlohmann::json;
namespace fs = std::filesystem;

constexpr int kFs = 48000;
constexpr long long kFrames = 4800;  // asset length (0.1 s)
constexpr float kStep = 0.0001f;     // mono ramp: frame k has value float(k) * kStep

// Per-process scratch directory (same pattern as test_main.cpp), removed at the end of main().
fs::path g_dir;

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// ---- assets ----

std::vector<float> ramp_frames() {
    std::vector<float> a(static_cast<size_t>(kFrames));
    for (long long k = 0; k < kFrames; ++k) a[static_cast<size_t>(k)] = static_cast<float>(k) * kStep;
    return a;
}

std::vector<float> reversed_frames(const std::vector<float>& a) {
    std::vector<float> r(a.size());
    for (size_t k = 0; k < a.size(); ++k) r[k] = a[a.size() - 1u - k];
    return r;
}

// Mono float32 WAV, hand-assembled (the engine writer is stereo-only). Reader-compatible with
// the canonical layout read_wav scans: 44-byte header, fmt tag 3, mono, 32-bit.
void write_mono_f32(const fs::path& path, int sr, const std::vector<float>& a) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t data_len = static_cast<uint32_t>(a.size()) * 4u;
    const uint32_t byte_rate = static_cast<uint32_t>(sr) * 4u;  // mono, 4 bytes per frame
    auto w32 = [&f](uint32_t v) {
        const uint8_t b[4] = {static_cast<uint8_t>(v & 0xFFu), static_cast<uint8_t>((v >> 8) & 0xFFu),
                              static_cast<uint8_t>((v >> 16) & 0xFFu), static_cast<uint8_t>((v >> 24) & 0xFFu)};
        f.write(reinterpret_cast<const char*>(b), 4);
    };
    auto w16 = [&f](uint16_t v) {
        const uint8_t b[2] = {static_cast<uint8_t>(v & 0xFFu), static_cast<uint8_t>((v >> 8) & 0xFFu)};
        f.write(reinterpret_cast<const char*>(b), 2);
    };
    f.write("RIFF", 4); w32(36u + data_len); f.write("WAVE", 4);
    f.write("fmt ", 4); w32(16); w16(3); w16(1); w32(static_cast<uint32_t>(sr));
    w32(byte_rate); w16(4); w16(32);
    f.write("data", 4); w32(data_len);
    f.write(reinterpret_cast<const char*>(a.data()), static_cast<std::streamsize>(data_len));
}

void write_stereo_f32(const fs::path& path, int sr, const std::vector<float>& lr) {
    op::wav::StereoBuffer b;
    b.sample_rate = sr;
    b.interleaved = lr;
    std::string err;
    if (!op::wav::write_wav_stereo(path.string(), b, op::wav::SampleFormat::F32, err)) {
        std::printf("FATAL: asset write failed: %s\n", err.c_str());
        std::exit(2);
    }
}

// ---- project builders (boring explicit JSON, one helper per shape) ----

// One sampler track (0 dB, pan 0), one note at beat 0. Voice keys only when asked for:
// reverse=true writes the key (the canonical `false` is never written by the model).
json sampler_project(const char* asset, bool reverse, double start_s, bool has_trim_end, double end_s,
                     int root_note, int note_pitch, double dur) {
    json t = json::object();
    t["name"] = "S";
    t["gain_db"] = 0.0;
    t["pan"] = 0.0;
    json v = json::object();
    v["kind"] = "sampler";
    v["path"] = asset;
    if (start_s != 0.0) v["start_s"] = start_s;
    if (has_trim_end) v["end_s"] = end_s;
    if (root_note >= 0) v["root_note"] = root_note;
    if (reverse) v["reverse"] = true;
    t["voice"] = std::move(v);
    json n = json::object();
    n["t"] = 0.0;
    n["dur"] = dur;
    if (note_pitch >= 0) n["note"] = note_pitch;
    json pat = json::object();
    pat["track"] = 0;
    pat["notes"] = json::array({n});
    json p = json::object();
    p["format_version"] = 0;
    p["bpm"] = 120.0;
    p["sample_rate"] = kFs;
    p["seed"] = 7;
    p["length_bars"] = 1.0;
    p["beats_per_bar"] = 4;
    p["tracks"] = json::array({t});
    p["patterns"] = json::array({pat});
    return p;
}

bool load_json(const json& pj, op::Project& out, std::string& err) {
    const fs::path opj = g_dir / "in.opj";
    write_file(opj, pj.dump(2) + "\n");
    return op::load_project(opj.string(), out, err);
}

std::string load_err(const json& pj) {
    op::Project p;
    std::string err;
    if (load_json(pj, p, err)) return "<loaded ok>";
    return err;
}

// Render at the scratch dir (asset paths are relative to it). FATAL on failure.
void render_json(const json& pj, op::wav::StereoBuffer& out) {
    op::Project p;
    std::string err;
    if (!load_json(pj, p, err)) {
        std::printf("FATAL: load failed: %s\n", err.c_str());
        std::exit(2);
    }
    if (!op::render_project(p, g_dir.string(), out, err)) {
        std::printf("FATAL: render failed: %s\n", err.c_str());
        std::exit(2);
    }
}

bool buffers_equal(const op::wav::StereoBuffer& x, const op::wav::StereoBuffer& y) {
    return x.sample_rate == y.sample_rate && x.interleaved == y.interleaved;
}

float L(const op::wav::StereoBuffer& b, long long frame) { return b.interleaved[static_cast<size_t>(frame) * 2u + 0u]; }
float R(const op::wav::StereoBuffer& b, long long frame) { return b.interleaved[static_cast<size_t>(frame) * 2u + 1u]; }

// The exact mix-path gain of a 0 dB pan-0 track (velocity 1.0 skips the copy, master 0 dB skips
// the multiply), so every expected sample below is `asset value * kPan` with exact float math.
float pan_gain() {
    float l, r;
    op::dsp::equal_power_pan(0.0f, l, r);
    return l;  // == r for pan 0
}

// Expected mono sample: the same expression chain render_sampler/add_stereo uses.
float expect1(float asset_value) { return asset_value * pan_gain(); }

// Expected repitch output of the reverse path: float(a + frac * (b - a)) in double, then the
// mix-target multiply (WP-0231 task §Render semantics, same rule as the forward path).
float expect_repitch(float rk, float rk1, double frac) {
    const double raw = static_cast<double>(rk) + frac * (static_cast<double>(rk1) - static_cast<double>(rk));
    return static_cast<float>(raw) * pan_gain();
}

std::string apply_err(const json& pj, const std::string& op_line) {
    op::Project p;
    std::string err;
    if (!load_json(pj, p, err)) return "<base load failed: " + err + ">";
    const std::string before = op::project_bytes(p);
    if (op::apply_op(p, json::parse(op_line), err)) return "<applied ok>";
    if (op::project_bytes(p) != before) return "<project changed by a rejected op!>";
    return err;
}

// ---- validation (parse rules of WP-0231 §Project JSON key) ----

constexpr const char* kMsgBool = "voice.reverse must be a boolean";
constexpr const char* kMsgSampler = "voice.reverse is only valid for sampler voices";

void test_validation() {
    CHECK(!op::Voice().reverse, "model.voice_default_false");

    // Absent key: loads, and the canonical save never mentions reverse.
    {
        op::Project p;
        std::string err;
        bool ok = load_json(sampler_project("ramp.wav", false, 0.0, false, 0.0, -1, -1, 0.25), p, err);
        const std::string bytes = ok ? op::project_bytes(p) : std::string();
        CHECK(ok && bytes.find("reverse") == std::string::npos, "parse.absent_key_no_reverse_in_save");
    }
    // "reverse": true parses, saves the key and round-trips stably.
    {
        op::Project p;
        std::string err;
        bool ok = load_json(sampler_project("ramp.wav", true, 0.0, false, 0.0, -1, -1, 0.25), p, err);
        const std::string b1 = ok ? op::project_bytes(p) : std::string();
        bool has_key = b1.find("\"reverse\": true") != std::string::npos;
        op::Project back;
        const fs::path opj = g_dir / "rt.opj";
        write_file(opj, b1);
        bool rt = op::load_project(opj.string(), back, err) && back.tracks[0].voice.reverse &&
                  op::project_bytes(back) == b1;
        CHECK(ok && p.tracks[0].voice.reverse && has_key && rt, "parse.true_saves_and_round_trips");
    }
    // Explicit "reverse": false loads and saves byte-identically to the never-key project.
    {
        json pj = sampler_project("ramp.wav", false, 0.0, false, 0.0, -1, -1, 0.25);
        pj["tracks"][0]["voice"]["reverse"] = false;
        op::Project p;
        std::string err;
        bool ok = load_json(pj, p, err);
        const std::string b_false = ok ? op::project_bytes(p) : std::string();
        op::Project q;
        bool ok2 = load_json(sampler_project("ramp.wav", false, 0.0, false, 0.0, -1, -1, 0.25), q, err);
        CHECK(ok && !p.tracks[0].voice.reverse && ok2 && b_false == op::project_bytes(q),
              "parse.false_omitted_and_byte_identical");
    }
    // Numbers (1, 0, 1.0), a string, null, an array and an object share one message on samplers.
    {
        bool all = true;
        const json bads = {1, 0, 1.0, "true", nullptr, json::array({true}), json::object()};
        for (const json& bad : bads) {
            json pj = sampler_project("ramp.wav", false, 0.0, false, 0.0, -1, -1, 0.25);
            pj["tracks"][0]["voice"]["reverse"] = bad;
            if (load_err(pj) != kMsgBool) all = false;
        }
        CHECK(all, "parse.every_non_boolean_exact_bool_msg");
    }
    // Key on a non-sampler voice: rule 1, exact message, even when the value is also wrong.
    {
        json pj = sampler_project("ramp.wav", false, 0.0, false, 0.0, -1, -1, 0.25);
        pj["tracks"][0]["voice"] = json{{"kind", "sine"}, {"reverse", true}};
        const std::string e1 = load_err(pj);
        pj["tracks"][0]["voice"] = json{{"kind", "sine"}, {"reverse", 1}};
        const std::string e2 = load_err(pj);
        CHECK(e1 == kMsgSampler && e2 == kMsgSampler, "parse.non_sampler_key_rule_wins_first");
    }
    // reverse stays parseable and renderable alongside root_note/trim (sampler keys coexist).
    {
        op::Project p;
        std::string err;
        bool ok = load_json(sampler_project("ramp.wav", true, 0.025, true, 0.075, 60, 67, 0.5), p, err);
        CHECK(ok && p.tracks[0].voice.reverse && p.tracks[0].voice.root_note == 60,
              "parse.reverse_coexists_with_root_and_trim");
    }
}

// ---- render semantics (WP-0231 §Render semantics) ----

void test_render() {
    const std::vector<float> a = ramp_frames();
    const std::vector<float> ar = reversed_frames(a);
    // i1 - 1 - k helper: the buffer frame of the reversed slice R[k] for the trim [480, 2880).
    const long long i0 = 480, i1 = 2880;  // start_s 0.01, end_s 0.06 — asymmetric on purpose
    const auto Rv = [&](long long k) { return a[static_cast<size_t>(i1 - 1 - k)]; };

    // Whole-asset reverse is bit-exact: reverse render == forward render of the pre-reversed asset.
    {
        op::wav::StereoBuffer r1, r2;
        render_json(sampler_project("ramp.wav", true, 0.0, false, 0.0, -1, -1, 3.0), r1);
        render_json(sampler_project("rev.wav", false, 0.0, false, 0.0, -1, -1, 3.0), r2);
        bool spot = L(r1, 0) == expect1(ar[0]) && L(r1, 1) == expect1(ar[1]) &&
                    L(r1, kFrames - 1) == expect1(ar[static_cast<size_t>(kFrames - 1)]) &&
                    L(r1, kFrames) == 0.0f;
        CHECK(buffers_equal(r1, r2) && spot, "render.whole_asset_reverse_bit_exact");
    }
    // Trim-then-reverse is bit-exact (pre-reversed asset + mirrored trim [kFrames-i1, kFrames-i0)),
    // and it is NOT the reverse-then-trim mapping (same trim on the pre-reversed asset).
    {
        op::wav::StereoBuffer right, wrong, via_rev;
        render_json(sampler_project("ramp.wav", true, 0.01, true, 0.06, -1, -1, 0.2), via_rev);
        render_json(sampler_project("rev.wav", false, 0.04, true, 0.09, -1, -1, 0.2), right);
        render_json(sampler_project("rev.wav", false, 0.01, true, 0.06, -1, -1, 0.2), wrong);
        bool spot = L(via_rev, 0) == expect1(Rv(0)) && L(via_rev, 1) == expect1(Rv(1)) &&
                    L(via_rev, i1 - i0 - 1) == expect1(Rv(i1 - i0 - 1)) && L(via_rev, i1 - i0) == 0.0f;
        CHECK(buffers_equal(via_rev, right) && spot, "render.trim_then_reverse_bit_exact");
        CHECK(!buffers_equal(via_rev, wrong), "render.trim_before_reverse_not_reverse_before_trim");
    }
    // A note shorter than the slice takes the slice's LAST frames first (frame 0 = buffer i1-1).
    {
        op::wav::StereoBuffer rev, fwd;
        render_json(sampler_project("ramp.wav", true, 0.01, true, 0.06, -1, -1, 0.05), rev);  // want 1200 < 2400
        render_json(sampler_project("ramp.wav", false, 0.01, true, 0.06, -1, -1, 0.05), fwd);
        bool tail = L(rev, 0) == expect1(Rv(0)) && L(rev, 1199) == expect1(Rv(1199)) && L(rev, 1200) == 0.0f;
        CHECK(tail && L(fwd, 0) == expect1(a[i0]) && L(fwd, 1199) == expect1(a[i0 + 1199]),
              "render.short_note_takes_slice_tail");
    }
    // Repitch +12 (ratio exactly 2.0) after reverse reads reversed slice frame 2n; the loop
    // breaks when k + 1 >= slice.
    {
        op::wav::StereoBuffer b;
        render_json(sampler_project("ramp.wav", true, 0.01, true, 0.06, 60, 72, 0.2), b);
        bool exact = L(b, 0) == expect1(Rv(0)) && L(b, 1) == expect1(Rv(2)) &&
                     L(b, 100) == expect1(Rv(200)) && L(b, 1199) == expect1(Rv(2398)) &&
                     L(b, 1200) == 0.0f;
        CHECK(exact, "render.repitch_plus12_reads_frame_2n");
    }
    // Repitch -12 (ratio exactly 0.5): even output frames land exactly on R[m]; odd ones are the
    // 0.5 interpolations between the REVERSED neighbours R[m], R[m+1].
    {
        op::wav::StereoBuffer b;
        render_json(sampler_project("ramp.wav", true, 0.01, true, 0.06, 60, 48, 0.2), b);
        bool even = L(b, 0) == expect1(Rv(0)) && L(b, 100) == expect1(Rv(50));
        bool odd = L(b, 1) == expect_repitch(Rv(0), Rv(1), 0.5) &&
                   L(b, 23) == expect_repitch(Rv(11), Rv(12), 0.5) &&
                   L(b, 4797) == expect_repitch(Rv(2398), Rv(2399), 0.5) &&
                   L(b, 4798) == 0.0f;
        CHECK(even && odd, "render.repitch_minus12_endpoints_and_reversed_interp");
    }
    // note == root_note: still the plain reversed slice (repitch path not taken, reverse still is).
    {
        op::wav::StereoBuffer b1, b2;
        render_json(sampler_project("ramp.wav", true, 0.0, false, 0.0, 67, 67, 3.0), b1);
        render_json(sampler_project("rev.wav", false, 0.0, false, 0.0, 67, 67, 3.0), b2);
        CHECK(buffers_equal(b1, b2) && L(b1, 0) == expect1(ar[0]), "render.note_eq_root_still_reverses");
    }
    // Stereo asset: both channels reverse the same way.
    {
        std::vector<float> lr(static_cast<size_t>(kFrames) * 2u);
        for (long long k = 0; k < kFrames; ++k) {
            lr[static_cast<size_t>(k) * 2u + 0u] = static_cast<float>(k) * kStep;
            lr[static_cast<size_t>(k) * 2u + 1u] = static_cast<float>(k) * (kStep * 2.0f);
        }
        std::vector<float> lrr(lr.size());
        for (long long k = 0; k < kFrames; ++k) {
            lrr[static_cast<size_t>(k) * 2u + 0u] = lr[static_cast<size_t>(kFrames - 1 - k) * 2u + 0u];
            lrr[static_cast<size_t>(k) * 2u + 1u] = lr[static_cast<size_t>(kFrames - 1 - k) * 2u + 1u];
        }
        write_stereo_f32(g_dir / "st.wav", kFs, lr);
        write_stereo_f32(g_dir / "strev.wav", kFs, lrr);
        op::wav::StereoBuffer b1, b2;
        render_json(sampler_project("st.wav", true, 0.0, false, 0.0, -1, -1, 3.0), b1);
        render_json(sampler_project("strev.wav", false, 0.0, false, 0.0, -1, -1, 3.0), b2);
        float l0 = lr[static_cast<size_t>(kFrames - 1) * 2u + 0u], r0 = lr[static_cast<size_t>(kFrames - 1) * 2u + 1u];
        CHECK(buffers_equal(b1, b2) && L(b1, 0) == expect1(l0) && R(b1, 0) == expect1(r0),
              "render.stereo_both_channels_reverse");
    }
    // reverse == false renders bit-identically to an absent key (root+pitch+trim coexist).
    {
        json pj = sampler_project("ramp.wav", false, 0.01, true, 0.06, 60, 64, 0.2);
        pj["tracks"][0]["voice"]["reverse"] = false;
        op::wav::StereoBuffer with_false, without;
        render_json(pj, with_false);
        render_json(sampler_project("ramp.wav", false, 0.01, true, 0.06, 60, 64, 0.2), without);
        CHECK(buffers_equal(with_false, without), "render.reverse_false_bit_identical_to_absent");
    }
    // Determinism (ADR-0004): the same reversed project rendered twice gives identical bytes.
    {
        op::wav::StereoBuffer b1, b2;
        const json pj = sampler_project("ramp.wav", true, 0.01, true, 0.06, 60, 67, 0.5);
        render_json(pj, b1);
        render_json(pj, b2);
        CHECK(buffers_equal(b1, b2), "render.deterministic_two_renders");
    }
}

// ---- the set_sampler_reverse op (WP-0231 §Operation) ----

void test_op() {
    const json base = sampler_project("ramp.wav", false, 0.01, true, 0.06, 60, 67, 0.5);
    const json handwritten = sampler_project("ramp.wav", true, 0.01, true, 0.06, 60, 67, 0.5);

    // Set true → the key lands in the canonical save, and applying the op then rendering equals
    // the hand-written reversed project byte for byte.
    {
        op::Project p;
        std::string err;
        bool ok = load_json(base, p, err);
        ok = ok && op::apply_op(p, json{{"op", "set_sampler_reverse"}, {"index", 0}, {"reverse", true}}, err);
        const std::string b_true = ok ? op::project_bytes(p) : std::string();
        op::Project h;
        bool ok_h = load_json(handwritten, h, err);
        CHECK(ok && b_true.find("\"reverse\": true") != std::string::npos && ok_h &&
                  b_true == op::project_bytes(h),
              "op.set_true_matches_handwritten_project");
    }
    // false and null both clear the flag; the saved bytes become the never-reversed project's.
    {
        op::Project p0;
        std::string err;
        bool ok = load_json(base, p0, err);
        const std::string b_never = op::project_bytes(p0);
        bool all = ok;
        for (const char* v : {"false", "null"}) {
            op::Project p = p0;
            all = all && op::apply_op(p, json::parse(std::string(R"({"op":"set_sampler_reverse","index":0,"reverse":)") + v + "}"), err);
            all = all && op::project_bytes(p) == b_never;
        }
        CHECK(all, "op.false_and_null_restore_never_reversed_bytes");
    }
    // Rejections: exact messages (with the "set_sampler_reverse: " prefix) and unchanged projects.
    CHECK(apply_err(base, R"({"op":"set_sampler_reverse","index":0,"reverse":true,"x":1})") ==
              "set_sampler_reverse: unknown argument 'x'",
          "op.reject_unknown_argument");
    CHECK(apply_err(base, R"({"op":"set_sampler_reverse","index":0})") ==
              "set_sampler_reverse: missing argument 'reverse'",
          "op.reject_missing_reverse");
    {
        bool all = true;
        for (const char* bad : {"1", "\"true\"", "1.0"}) {
            const std::string e = apply_err(base, std::string(R"({"op":"set_sampler_reverse","index":0,"reverse":)") + bad + "}");
            if (e != "set_sampler_reverse: 'reverse' must be a boolean or null") all = false;
        }
        CHECK(all, "op.reject_reverse_not_boolean_or_null");
    }
    CHECK(apply_err(base, R"({"op":"set_sampler_reverse","index":5,"reverse":true})") ==
              "set_sampler_reverse: 'index' out of range",
          "op.reject_index_out_of_range");
    CHECK(apply_err(base, R"({"op":"set_sampler_reverse","index":0.0,"reverse":true})") ==
              "set_sampler_reverse: 'index' must be an integer",
          "op.reject_index_float");
    {
        json sine = sampler_project("ramp.wav", false, 0.0, false, 0.0, -1, -1, 0.25);
        sine["tracks"][0]["voice"] = json{{"kind", "sine"}, {"freq_hz", 440.0}};
        const bool t = apply_err(sine, R"({"op":"set_sampler_reverse","index":0,"reverse":true})") ==
                       "set_sampler_reverse: track is not a sampler voice";
        const bool n = apply_err(sine, R"({"op":"set_sampler_reverse","index":0,"reverse":null})") ==
                       "set_sampler_reverse: track is not a sampler voice";
        CHECK(t && n, "op.reject_non_sampler_including_null");
    }
    // Order of checks: an unknown key wins over a bad index, the bad index over the missing value.
    CHECK(apply_err(base, R"({"op":"set_sampler_reverse","index":9,"reverse":1,"x":1})") ==
              "set_sampler_reverse: unknown argument 'x'",
          "op.check_order_keys_before_index");
    CHECK(apply_err(base, R"({"op":"set_sampler_reverse","index":9})") ==
              "set_sampler_reverse: 'index' out of range",
          "op.check_order_index_before_reverse");
}

// ---- opd diff support (WP-0231 §opd diff) ----

void test_diff() {
    const json base = sampler_project("ramp.wav", false, 0.01, true, 0.06, 60, 67, 0.5);
    const json rev = sampler_project("ramp.wav", true, 0.01, true, 0.06, 60, 67, 0.5);
    op::Project a, b;
    std::string err;
    if (!load_json(base, a, err) || !load_json(rev, b, err)) {
        std::printf("FATAL: diff test base load failed: %s\n", err.c_str());
        std::exit(2);
    }

    // never → reversed: exactly the one quoted journal line, and applying it reproduces b's bytes.
    {
        std::vector<json> ops;
        bool ok = op::diff_projects(a, b, ops, err);
        bool exact = ok && ops.size() == 1 &&
                     ops[0].dump() == "{\"index\":0,\"op\":\"set_sampler_reverse\",\"reverse\":true}";
        std::string journal = exact ? ops[0].dump() + "\n" : "";
        op::Project work = a;
        bool repro = exact && op::apply_journal(work, journal, err) && op::project_bytes(work) == op::project_bytes(b);
        CHECK(exact && repro, "diff.emits_true_line_and_reproduces_b");
    }
    // reversed → never: the false line (never null), and the journal reproduces a's bytes.
    {
        std::vector<json> ops;
        bool ok = op::diff_projects(b, a, ops, err);
        bool exact = ok && ops.size() == 1 &&
                     ops[0].dump() == "{\"index\":0,\"op\":\"set_sampler_reverse\",\"reverse\":false}";
        op::Project work = b;
        bool repro = exact && op::apply_journal(work, ops[0].dump() + "\n", err) &&
                     op::project_bytes(work) == op::project_bytes(a);
        CHECK(exact && repro, "diff.emits_false_line_and_reproduces_a");
    }
    // Equal projects (reversed vs reversed): no ops at all.
    {
        op::Project b2;
        bool ok_l = load_json(rev, b2, err);
        std::vector<json> ops;
        bool ok = ok_l && op::diff_projects(b, b2, ops, err);
        CHECK(ok && ops.empty(), "diff.equal_projects_emit_nothing");
    }
    // A voice difference in anything else stays unsupported — even when both sides are reversed.
    {
        json other = sampler_project("ramp.wav", true, 0.01, true, 0.06, 62, 67, 0.5);  // root_note 62 vs 60
        op::Project o;
        bool ok_l = load_json(other, o, err);
        std::vector<json> ops;
        bool rejected = ok_l && !op::diff_projects(b, o, ops, err) &&
                        err == "unsupported difference: tracks[0].voice" && ops.empty();
        CHECK(rejected, "diff.root_note_difference_still_unsupported");
    }
    // Step-5 order: the track ops come out name, gain, pan, mute, solo, choke, reverse —
    // set_sampler_reverse LAST, byte-exactly.
    {
        json changed = base;
        changed["tracks"][0]["name"] = "Reversed";
        changed["tracks"][0]["gain_db"] = 2.0;
        changed["tracks"][0]["pan"] = 0.5;
        changed["tracks"][0]["mute"] = false;   // explicit false == default: must emit nothing
        changed["tracks"][0]["solo"] = true;
        changed["tracks"][0]["choke_group"] = 3;
        changed["tracks"][0]["voice"]["reverse"] = true;
        op::Project c;
        bool ok_l = load_json(changed, c, err);
        std::vector<json> ops;
        bool ok = ok_l && op::diff_projects(a, c, ops, err);
        bool seq = ok && ops.size() == 6 &&
                   ops[0].dump() == "{\"index\":0,\"name\":\"Reversed\",\"op\":\"rename_track\"}" &&
                   ops[1].dump() == "{\"gain_db\":2.0,\"index\":0,\"op\":\"set_track_gain\"}" &&
                   ops[2].dump() == "{\"index\":0,\"op\":\"set_track_pan\",\"pan\":0.5}" &&
                   ops[3].dump() == "{\"index\":0,\"op\":\"set_track_solo\",\"solo\":true}" &&
                   ops[4].dump() == "{\"group\":3,\"index\":0,\"op\":\"set_choke_group\"}" &&
                   ops[5].dump() == "{\"index\":0,\"op\":\"set_sampler_reverse\",\"reverse\":true}";
        CHECK(seq, "diff.step5_order_ends_with_set_sampler_reverse");
    }
}

// ---- undo/redo and journal atomicity (existing History, no new mechanism) ----

void test_history_journal() {
    const json base = sampler_project("ramp.wav", false, 0.0, false, 0.0, -1, -1, 0.25);
    op::Project p;
    std::string err;
    if (!load_json(base, p, err)) {
        std::printf("FATAL: history test base load failed: %s\n", err.c_str());
        std::exit(2);
    }
    const std::string b_never = op::project_bytes(p);

    op::History h;
    const json set_true = json{{"op", "set_sampler_reverse"}, {"index", 0}, {"reverse", true}};
    bool applied = h.apply(p, set_true, err);
    const std::string b_true = op::project_bytes(p);
    const bool undone = h.undo(p) && op::project_bytes(p) == b_never;
    const bool redone = h.redo(p) && op::project_bytes(p) == b_true;
    CHECK(applied && b_true != b_never && undone && redone, "history.undo_redo_byte_exact");

    // A rejected apply changes nothing: project bytes and both stacks untouched.
    {
        const std::size_t depth = h.undo_depth();
        const bool before_redo = h.can_redo();
        const bool rejected = !h.apply(p, json{{"op", "set_sampler_reverse"}, {"index", 0}, {"reverse", 1}}, err) &&
                              op::project_bytes(p) == b_true && h.undo_depth() == depth && h.can_redo() == before_redo;
        CHECK(rejected, "history.rejected_apply_changes_nothing");
    }
    // apply_journal: a bad line is rejected atomically (phase kRejected, nothing applied).
    {
        const std::string journal =
            "{\"op\":\"set_sampler_reverse\",\"index\":0,\"reverse\":true}\n"
            "{\"op\":\"set_sampler_reverse\",\"index\":0,\"reverse\":1}\n";
        op::Project work = p;
        op::JournalOutcome oc{};
        const bool atomic = !op::apply_journal(work, journal, err, &oc) && oc.failure == op::JournalFailure::kRejected &&
                            op::project_bytes(work) == op::project_bytes(p);
        CHECK(atomic, "journal.bad_line_rejected_atomically");
    }
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / ("op_sampler_reverse_" + std::to_string(static_cast<long>(::getpid())));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FATAL: cannot create temp dir %s\n", g_dir.string().c_str());
        return 2;
    }

    const std::vector<float> a = ramp_frames();
    write_mono_f32(g_dir / "ramp.wav", kFs, a);
    write_mono_f32(g_dir / "rev.wav", kFs, reversed_frames(a));

    test_validation();
    test_render();
    test_op();
    test_diff();
    test_history_journal();

    fs::remove_all(g_dir, ec);
    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures;
}
