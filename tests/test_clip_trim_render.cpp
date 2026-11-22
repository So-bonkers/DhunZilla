// Unit tests for clip trim applied at render (docs/spec/project-model.md §Clip trim — render, WP-0193b).
// Projects are built in memory with nlohmann::json + op::project_from_json, rendered with
// op::render_project and compared bit-exactly (std::memcmp), no tolerances. Boring assert-based runner,
// own main; exit code = number of failures. Run: ./build/op_clip_trim_render_tests
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/model_json.hpp"
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

constexpr int kFs = 48000;         // 120 bpm → exactly 24000 frames per beat
constexpr long long kF48 = 240;    // choke fade at 48 kHz: max(1, 48000 * 5 / 1000)
std::string g_dir;                 // unique scratch dir (choke assets), removed in main()

void fatal(const std::string& m) { std::printf("FATAL: %s\n", m.c_str()); std::exit(2); }
json nt(double t, double d) { return json{{"t", t}, {"dur", d}}; }
json cl(int p, double bar, int reps) { return json{{"pattern", p}, {"start_bar", bar}, {"repeats", reps}}; }

// One sine track (0 dB centred), one pattern on it, given arrangement; 4 bars @ 120 bpm.
json sine_proj(json notes, json clips, double swing = 0.0) {
    json pat = json{{"track", 0}, {"notes", std::move(notes)}};
    if (swing != 0.0) pat["swing"] = swing;
    return json{{"format_version", 0}, {"bpm", 120.0}, {"sample_rate", kFs}, {"seed", 7},
                {"length_bars", 4.0}, {"beats_per_bar", 4},
                {"tracks", json::array({json{{"name", "L"}, {"voice", {{"kind", "sine"}, {"freq_hz", 437.0}}}}})},
                {"patterns", json::array({std::move(pat)})}, {"arrangement", std::move(clips)}};
}

op::wav::StereoBuffer render(const json& pj) {
    op::Project p; std::string err;
    if (!op::project_from_json(pj, p, err)) fatal("parse: " + err);
    op::wav::StereoBuffer out;
    if (!op::render_project(p, g_dir, out, err)) fatal("render: " + err);
    return out;
}

bool eq_buf(const op::wav::StereoBuffer& a, const op::wav::StereoBuffer& b) {
    return a.interleaved.size() == b.interleaved.size() &&
           std::memcmp(a.interleaved.data(), b.interleaved.data(), a.interleaved.size() * sizeof(float)) == 0;
}
bool all_zero(const op::wav::StereoBuffer& b) {
    for (float s : b.interleaved) if (s != 0.0f) return false;
    return true;
}
// a equals b shifted left by `shift` frames (a[f] == b[f + shift]) with a zero tail.
bool eq_shift(const op::wav::StereoBuffer& a, const op::wav::StereoBuffer& b, long long shift) {
    if (a.interleaved.size() != b.interleaved.size()) return false;
    const size_t s = static_cast<size_t>(shift) * 2u, n = a.interleaved.size();
    if (n < s || std::memcmp(a.interleaved.data(), b.interleaved.data() + s, (n - s) * sizeof(float)) != 0)
        return false;
    for (size_t i = n - s; i < n; ++i) if (a.interleaved[i] != 0.0f) return false;
    return true;
}

// The E=4 project (notes t=0..3, dur 1), one clip of pattern 0 at bar `bar` with `reps` repeats and
// window offset w0 / length len set only when > 0 (len <= 0 ⇒ key absent → derived length).
json win(double w0, double len, double bar = 0.0, int reps = 3) {
    json j = sine_proj(json::array({nt(0, 1), nt(1, 1), nt(2, 1), nt(3, 1)}), json::array({cl(0, bar, reps)}));
    if (w0 > 0.0) j["arrangement"][0]["offset_beats"] = w0;
    if (len > 0.0) j["arrangement"][0]["length_beats"] = len;
    return j;
}

// ---- §1 window semantics ----
void test_windows() {
    const auto A = render(win(0, -1));  // untrimmed: material [0,12), three repeats
    json e = win(0, -1);
    e["arrangement"][0]["offset_beats"] = 0.0;   // explicit zero stays the untrimmed path
    CHECK(eq_buf(A, render(e)), "untrimmed.explicit_offset_zero_identity");
    CHECK(eq_buf(A, render(win(0, 12))), "untrimmed.length_equals_material_identity");
    CHECK(eq_buf(render(sine_proj(json::array({nt(0, 1), nt(1, 1), nt(2, 1), nt(3, 1)}),
                                   json::array({cl(0, 0.0, 1)}))), render(win(4, 4))),
          "window.one_whole_repeat_equals_single");   // whole-repeat window == one untrimmed repeat

    // Partial window [2,8) spanning repeats == flattened equivalent (pins base + (s - w0)).
    const json flat = sine_proj(json::array({nt(0,1), nt(1,1), nt(2,1), nt(3,1), nt(4,1), nt(5,1)}),
                                json::array({cl(0, 0.0, 1)}));
    CHECK(eq_buf(render(win(2, 6)), render(flat)), "window.spanning_repeats_equals_flattened");

    // Window into a later repeat of a clip at bar 1: base 4, window [6,9) → notes at beats 4,5,6.
    const json flat2 = sine_proj(json::array({nt(0, 1), nt(1, 1), nt(2, 1)}), json::array({cl(0, 1.0, 1)}));
    CHECK(eq_buf(render(win(6, 3, 1.0)), render(flat2)), "window.starts_in_later_repeat_at_bar1");

    // A length past the material adds no audio: window [3,13) == window [3,12).
    CHECK(eq_buf(render(win(3, 10)), render(win(3, 9))), "length.past_material_adds_nothing");
    // Empty window: derived len == 0 (w0 = material end) and len < 0 both give complete silence.
    CHECK(all_zero(render(win(12, -1))), "empty.len_zero_all_silence");
    CHECK(all_zero(render(win(20, -1))), "empty.len_negative_all_silence");
}

// Swing keys off pattern-local n.t, never the shifted position: the swung note at t=2.75 in window
// [1,3.25) lands exactly offset frames (24000) left of its untrimmed placement.
void test_swing_shift() {
    const json n = json::array({nt(2.75, 0.5)});  // E = 3.25; t=2.75 is an odd 16th → swung
    const auto u = render(sine_proj(n, json::array({cl(0, 0.0, 1)}), 0.25));
    json j = sine_proj(n, json::array({cl(0, 0.0, 1)}), 0.25);
    j["arrangement"][0]["offset_beats"] = 1.0;
    j["arrangement"][0]["length_beats"] = 2.25;   // window [1,3.25) contains s=2.75
    CHECK(eq_shift(render(j), u, kFs / 2), "swing.equals_untrimmed_shifted_by_offset_frames");
}

// Fades end at base + len: trimmed clips equal an untrimmed pattern whose extent is exactly len.
void test_fades() {
    json ref = sine_proj(json::array({nt(0, 1), nt(1, 1)}), json::array({cl(0, 0.0, 1)}));  // E' = 2
    ref["arrangement"][0]["fade_out_beats"] = 1.0;   // untrimmed end: base + extent = 2
    json a = win(0, 2);                               // window [0,2), fade ends at base + len = 2
    a["arrangement"][0]["fade_out_beats"] = 1.0;
    CHECK(eq_buf(render(a), render(ref)), "fade.ends_at_base_plus_len");
    json b = win(1, 2);                               // window [1,3): notes land at beats 0,1 anyway
    b["arrangement"][0]["fade_out_beats"] = 1.0;
    CHECK(eq_buf(render(b), render(ref)), "fade.with_offset_ends_at_base_plus_len");
}

// Choke pre-pass respects the window: a trimmed-out note neither sounds nor cuts another track.
void test_choke() {
    auto write_asset = [&](const char* name, float v, long long frames) {
        op::wav::StereoBuffer b;
        b.sample_rate = kFs;
        b.interleaved.assign(static_cast<size_t>(frames) * 2u, v);
        std::string err;
        if (!op::wav::write_wav_stereo((fs::path(g_dir) / name).string(), b, op::wav::SampleFormat::F32, err))
            fatal("asset write: " + err);
    };
    write_asset("long.wav", 0.5f, 192000);   // hard left "Open": rings 3.5 beats
    write_asset("short.wav", 0.25f, 48000);  // hard right "Chk": triggers at t=0.125 and t=2.0
    auto sampler = [](const char* name, double pan, const char* path) {
        return json{{"name", name}, {"gain_db", 0.0}, {"pan", pan}, {"choke_group", 1},
                    {"voice", {{"kind", "sampler"}, {"path", path}}}};
    };
    json p = json{{"format_version", 0}, {"bpm", 120.0}, {"sample_rate", kFs}, {"seed", 9},
                  {"length_bars", 4.0}, {"beats_per_bar", 4},
                  {"tracks", json::array({sampler("Open", -1.0, "long.wav"), sampler("Chk", 1.0, "short.wav")})},
                  {"patterns", json::array({json{{"track", 0}, {"notes", json::array({nt(0.0, 3.5)})}},
                                            json{{"track", 1}, {"notes", json::array({nt(0.125, 0.25), nt(2.0, 0.25)})}}})},
                  {"arrangement", json::array({cl(0, 0.0, 1), cl(1, 0.0, 1)})}};

    // Untrimmed: the t=0.125 trigger (frame 3000) cuts Open after the fade at frame 3240.
    const auto u = render(p);
    CHECK(u.interleaved[3240 * 2] == 0.0f && u.interleaved[(3000 + kF48 - 1) * 2] != 0.0f,
          "choke.untrimmed_variant_cuts_early");

    // Window [0.5, len): t=0.125 is trimmed out — Open plays on past frame 3240 at full level (the
    // hard-panned asset value through unity gain) and is cut only by the surviving t=2.0 trigger,
    // itself placed at beat 2.0 - 0.5 = 1.5 (frame 36000).
    p["arrangement"][1]["offset_beats"] = 0.5;
    const auto t = render(p);
    CHECK(t.interleaved[3240 * 2] == 0.5f, "choke.excluded_note_does_not_cut");
    CHECK(t.interleaved[(36000 + kF48) * 2] == 0.0f && t.interleaved[(36000 + kF48 - 1) * 2] != 0.0f,
          "choke.surviving_trigger_cuts");
}

}  // namespace

int main() {
    g_dir = (fs::temp_directory_path() / ("dz_clip_trim_render_" + std::to_string(::getpid()))).string();
    fs::create_directories(g_dir);
    test_windows();
    test_swing_shift();
    test_fades();
    test_choke();
    fs::remove_all(g_dir);
    return g_failures;
}
