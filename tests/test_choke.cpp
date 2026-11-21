// Unit tests for sampler choke groups (docs/spec/project-model.md §Sampler choke groups and
// docs/spec/operations.md §Choke group operation, WP-0181). Boring assert-based runner — no
// framework deps. Exit code = number of failures. Run: ./build/op_choke_tests
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

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
constexpr float kVLong = 0.5f;    // constant sample value of the long "open hat" asset
constexpr float kVShort = 0.25f;  // constant sample value of the short "closed hat" asset
constexpr long long kF48 = 240;   // 5 ms fade at 48 kHz: max(1, 48000 * 5 / 1000)

// Per-process scratch directory (same pattern as test_main.cpp), removed at the end of main().
fs::path g_dir;

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

// Write a stereo float32 asset of `frames` frames with constant value v on both channels.
void write_const_asset(const fs::path& path, int sr, float v, long long frames) {
    op::wav::StereoBuffer b;
    b.sample_rate = sr;
    b.interleaved.resize(static_cast<size_t>(frames) * 2u);
    for (long long i = 0; i < frames; ++i) {
        b.interleaved[static_cast<size_t>(i) * 2u + 0u] = v;
        b.interleaved[static_cast<size_t>(i) * 2u + 1u] = v;
    }
    std::string err;
    if (!op::wav::write_wav_stereo(path.string(), b, op::wav::SampleFormat::F32, err)) {
        std::printf("FATAL: asset write failed: %s\n", err.c_str());
        std::exit(2);
    }
}

// ---- project builders (boring explicit JSON, one helper per shape) ----

json mk_note(double t, double dur) {
    json n = json::object();
    n["t"] = t;
    n["dur"] = dur;
    return n;
}

json mk_sampler_track(const char* name, double pan, const char* path, int group) {
    json t = json::object();
    t["name"] = name;
    t["gain_db"] = 0.0;
    t["pan"] = pan;
    if (group != 0) t["choke_group"] = group;
    json v = json::object();
    v["kind"] = "sampler";
    v["path"] = path;
    t["voice"] = std::move(v);
    return t;
}

// Base render project (48 kHz, 120 bpm → spb 24000): Open (long asset, hard left) one note at
// beat 0 ringing 9600 frames; Closed (short asset, hard right) one note at beat 0.125 = frame
// 3000. With both in group 1 the Open voice is cut at 3000; without groups it rings to its end.
json base_project(int group_open, int group_closed) {
    json p = json::object();
    p["format_version"] = 0;
    p["bpm"] = 120.0;
    p["sample_rate"] = kFs;
    p["seed"] = 7;
    p["length_bars"] = 4.0;
    p["beats_per_bar"] = 4;
    p["tracks"] = json::array({mk_sampler_track("Open", -1.0, "long.wav", group_open),
                               mk_sampler_track("Closed", 1.0, "short.wav", group_closed)});
    json pat0 = json::object();
    pat0["track"] = 0;
    pat0["notes"] = json::array({mk_note(0.0, 3.5)});
    json pat1 = json::object();
    pat1["track"] = 1;
    pat1["notes"] = json::array({mk_note(0.125, 0.25)});
    p["patterns"] = json::array({pat0, pat1});
    return p;
}

// Load `pj` from a temp .opj and render the mix. Returns false (err set) on any failure.
bool render_json(const json& pj, op::wav::StereoBuffer& out, std::string& err) {
    const fs::path opj = g_dir / "render.opj";
    write_file(opj, pj.dump(2) + "\n");
    op::Project p;
    if (!op::load_project(opj.string(), p, err)) return false;
    return op::render_project(p, g_dir.string(), out, err);
}

float L(const op::wav::StereoBuffer& b, long long frame) { return b.interleaved[static_cast<size_t>(frame) * 2u + 0u]; }
float R(const op::wav::StereoBuffer& b, long long frame) { return b.interleaved[static_cast<size_t>(frame) * 2u + 1u]; }

bool nearf(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// Spec multiplication order for a faded voice at fade offset d: (sample * track_gain_channel) * g
// with g = float(F - d) / float(F). Track gain here is 0 dB hard-pan = exactly 1.0f.
float expect_fade(float sample, long long F, long long d) {
    return (sample * 1.0f) * (static_cast<float>(F - d) / static_cast<float>(F));
}

// ---- validation (load_project error strings are the whole message) ----

std::string load_err(const json& pj) {
    const fs::path opj = g_dir / "check.opj";
    write_file(opj, pj.dump(2) + "\n");
    op::Project p;
    std::string err;
    if (op::load_project(opj.string(), p, err)) return "<loaded ok>";
    return err;
}

constexpr const char* kMsgRange = "track.choke_group must be an integer in [1, 16]";
constexpr const char* kMsgSampler = "track.choke_group is only valid for sampler tracks";

void test_validation() {
    // Every non-(integer 1..16) value gives the exact range message (spec rule 1).
    bool all_bad = true;
    const json bads = {"1", 1.0, nullptr, true, 0, 17, -1};
    for (const json& bad : bads) {
        json pj = base_project(1, 1);
        pj["tracks"][0]["choke_group"] = bad;
        if (load_err(pj) != kMsgRange) all_bad = false;
    }
    CHECK(all_bad, "validation.bad_kinds_exact_msg");

    // Order: the solo check runs first (checked right after solo, before the voice).
    {
        json pj = base_project(99, 0);
        pj["tracks"][0]["solo"] = 1;  // not a boolean
        CHECK(load_err(pj) == "track.solo must be a boolean", "validation.order_solo_first");
    }
    // Order: the range check runs before the voice parse (bad group + bad voice → range message).
    {
        json pj = base_project(99, 0);
        pj["tracks"][0]["voice"]["kind"] = "nope";
        CHECK(load_err(pj) == kMsgRange, "validation.range_before_voice");
    }
    // Rule 2: a valid group on a non-sampler track (checked after the voice parsed).
    {
        json pj = base_project(1, 0);
        json v = json::object();
        v["kind"] = "sine";
        pj["tracks"][0]["voice"] = v;
        CHECK(load_err(pj) == kMsgSampler, "validation.non_sampler");
    }
    // Boundary values 1 and 16 load fine.
    {
        const fs::path opj = g_dir / "bounds.opj";
        write_file(opj, base_project(1, 16).dump(2) + "\n");
        op::Project p;
        std::string err;
        CHECK(op::load_project(opj.string(), p, err), "validation.accepts_1_and_16");
    }

    // Canonical save: key omitted by default; written (after solo, before voice) and round-tripped.
    {
        op::Project none;
        const fs::path opj = g_dir / "save_none.opj";
        write_file(opj, base_project(0, 0).dump(2) + "\n");
        std::string err;
        bool ok = op::load_project(opj.string(), none, err);
        const std::string bytes_none = op::project_bytes(none);
        op::Project grouped;
        const fs::path opj2 = g_dir / "save_grouped.opj";
        write_file(opj2, base_project(3, 0).dump(2) + "\n");
        ok = ok && op::load_project(opj2.string(), grouped, err);
        const std::string bytes3 = op::project_bytes(grouped);
        // Round-trip: saving the canonical bytes and re-loading yields identical bytes.
        op::Project back;
        const fs::path opj3 = g_dir / "save_back.opj";
        write_file(opj3, bytes3);
        bool round_trip = ok && op::load_project(opj3.string(), back, err) &&
                          op::project_bytes(back) == bytes3;
        // Put solo on track 0 too and check the key lands in its canonical dump position (keys
        // dump alphabetically, as everywhere else in this format: choke_group precedes gain_db).
        grouped.tracks[0].solo = true;
        const std::string bytes_solo = op::project_bytes(grouped);
        CHECK(ok && bytes_none.find("choke_group") == std::string::npos, "save.omits_by_default");
        const size_t i_cg = bytes_solo.find("\"choke_group\"");
        const size_t i_gain = bytes_solo.find("\"gain_db\"");
        CHECK(bytes3.find("\"choke_group\": 3") != std::string::npos &&
              i_cg != std::string::npos && i_gain != std::string::npos && i_cg < i_gain,
              "save.writes_key_when_set");
        CHECK(round_trip, "save.round_trips_bytes");
    }
}

// ---- render semantics ----

void test_render() {
    std::string err;

    // Base cut at 48 kHz: fade exactly linear over 240 frames starting at frame c = 3000.
    const long long c48 = static_cast<long long>(std::llround(0.125 * (static_cast<double>(kFs) * 60.0 / 120.0)));
    op::wav::StereoBuffer grouped;
    bool ok = render_json(base_project(1, 1), grouped, err);
    CHECK(ok, "render.grouped_ok");
    if (ok) {
        bool fade_exact = L(grouped, c48 - 1) == kVLong && L(grouped, c48) == expect_fade(kVLong, kF48, 0);
        for (long long d = 0; d < kF48; ++d)
            if (!nearf(L(grouped, c48 + d), expect_fade(kVLong, kF48, d), 1e-9f)) fade_exact = false;
        CHECK(fade_exact, "render.fade_240_frames_linear");
        CHECK(L(grouped, c48 + kF48) == 0.0f && L(grouped, c48 + kF48 + 500) == 0.0f,
              "render.silent_after_fade");
        // The triggering voice plays normally on its own track (hard right): no cut there.
        CHECK(R(grouped, c48) == kVShort && R(grouped, c48 + kF48) == kVShort,
              "render.trigger_unaffected");
    }

    // No group → rings to the natural end; different groups → ignored.
    {
        op::wav::StereoBuffer nogroup, other;
        const bool a = render_json(base_project(0, 0), nogroup, err);
        const bool b = render_json(base_project(1, 2), other, err);
        CHECK(a && b && L(nogroup, c48 + kF48) == kVLong && L(other, c48 + kF48) == kVLong,
              "render.no_cut_without_group_or_other_group");
    }

    // Notes of the same track never cut each other: two notes on track 0 (second at frame 3000),
    // no note on track 1. At frame c+F both voices of track 0 still sound → L = 0.5 + 0.5.
    {
        json pj = base_project(1, 1);
        pj["patterns"][0]["notes"] = json::array({mk_note(0.0, 3.5), mk_note(0.125, 3.5)});
        pj["patterns"][1]["notes"] = json::array();
        op::wav::StereoBuffer out;
        if (render_json(pj, out, err))
            CHECK(nearf(L(out, c48 + kF48), 2.0f * kVLong, 1e-6f), "render.same_track_never_cuts");
        else
            CHECK(false, "render.same_track_never_cuts");
    }

    // A trigger on the very same frame does not cut (both simultaneous hits sound).
    {
        json pj = base_project(1, 1);
        pj["patterns"][0]["notes"] = json::array({mk_note(0.125, 3.5)});  // both start at frame 3000
        op::wav::StereoBuffer out;
        if (render_json(pj, out, err))
            CHECK(L(out, c48 + kF48) == kVLong && R(out, c48 + kF48) == kVShort,
                  "render.same_frame_no_cut");
        else
            CHECK(false, "render.same_frame_no_cut");
    }

    // A trigger after the voice's natural end changes nothing (voice simply ends).
    {
        json pj = base_project(1, 1);
        pj["patterns"][0]["notes"] = json::array({mk_note(0.0, 0.05)});  // want = 1200 frames < c
        op::wav::StereoBuffer out;
        if (render_json(pj, out, err))
            CHECK(L(out, 1100) == kVLong && L(out, 1200) == 0.0f, "render.trigger_after_voice_end");
        else
            CHECK(false, "render.trigger_after_voice_end");
    }

    // Earliest trigger wins: two Closed notes (frames 3000 and 6000) → one fade from 3000 only.
    {
        json pj = base_project(1, 1);
        pj["patterns"][1]["notes"] = json::array({mk_note(0.125, 0.25), mk_note(0.25, 0.25)});
        op::wav::StereoBuffer out;
        if (render_json(pj, out, err))
            CHECK(L(out, c48 + kF48) == 0.0f && L(out, 6000 + 100) == 0.0f, "render.earliest_trigger_wins");
        else
            CHECK(false, "render.earliest_trigger_wins");
    }

    // A muted track neither sounds nor triggers (spec §mixer audibility).
    {
        json pj = base_project(1, 1);
        pj["tracks"][1]["mute"] = true;
        op::wav::StereoBuffer out;
        if (render_json(pj, out, err))
            CHECK(L(out, c48 + kF48) == kVLong && R(out, c48 + 100) == 0.0f, "render.muted_no_trigger");
        else
            CHECK(false, "render.muted_no_trigger");
    }

    // Clips, repeats and swing: trigger frames come from the renderer's placement math.
    // Open clip at bar 1 → voice spans frames 96000..105599. Closed pattern (extent 1.0 beat,
    // swing 0.5 → its odd-16th note at t 0.75 lands 0.0625 beats late) placed by a clip at bar
    // 0.6 with repeats = 2: repeat 0 triggers at (2.4 + 0.8125) * 24000 = 77100 (before the Open
    // voice starts → cannot cut), repeat 1 at (3.4 + 0.8125) * 24000 = 101100 → cuts there.
    // Without swing the trigger would sit at frame 99600 — checked to still be full level.
    {
        json pj = base_project(1, 1);
        pj["patterns"][0]["notes"] = json::array({mk_note(0.0, 3.0)});
        pj["patterns"][1]["swing"] = 0.5;
        pj["patterns"][1]["notes"] = json::array({mk_note(0.75, 0.25)});
        json clip0 = json::object();
        clip0["pattern"] = 0;
        clip0["start_bar"] = 1.0;
        clip0["repeats"] = 1;
        json clip1 = json::object();
        clip1["pattern"] = 1;
        clip1["start_bar"] = 0.6;
        clip1["repeats"] = 2;
        pj["arrangement"] = json::array({clip0, clip1});
        // Mirror the renderer's own double math for the r=1 placement (clip base + extent + note t
        // + swing, then llround(beats * spb_const)): exactly frame 101100.
        const double base_beats = 0.6 * 4.0;               // clip start_bar × beats_per_bar
        const double off_r1 = base_beats + 1.0 * 1.0;      // repeat r=1, extent 1.0 beat
        const double t_abs = off_r1 + 0.75 + 0.5 * 0.125;  // note t + swing on the odd 16th
        const long long c_clip = std::llround(t_abs * (static_cast<double>(kFs) * 60.0 / 120.0));
        op::wav::StereoBuffer out;
        if (render_json(pj, out, err)) {
            bool good = c_clip == 101100 && L(out, 99600) == kVLong && L(out, c_clip - 1) == kVLong &&
                        nearf(L(out, c_clip + kF48 / 2), expect_fade(kVLong, kF48, kF48 / 2), 1e-9f) &&
                        L(out, c_clip + kF48) == 0.0f;
            CHECK(good, "render.clips_repeats_swing");
        } else {
            CHECK(false, "render.clips_repeats_swing");
        }
    }

    // A group with a single track renders byte-identically to no group at all.
    {
        op::wav::StereoBuffer solo_g, plain;
        const bool a = render_json(base_project(5, 0), solo_g, err);
        const bool b = render_json(base_project(0, 0), plain, err);
        CHECK(a && b && solo_g.interleaved == plain.interleaved, "render.single_track_group_identical");
    }

    // Stems see exactly the same cuts as the mix (triggers evaluated over the whole project).
    {
        const fs::path opj = g_dir / "stems.opj";
        write_file(opj, base_project(1, 1).dump(2) + "\n");
        op::Project p;
        std::vector<op::wav::StereoBuffer> stems;
        op::wav::StereoBuffer mix;
        bool ok = op::load_project(opj.string(), p, err) && op::render_stems(p, g_dir.string(), stems, mix, err);
        bool good = ok && stems.size() == 2;
        if (good) {
            for (long long d = 0; d < kF48; ++d)
                if (!nearf(L(stems[0], c48 + d), L(mix, c48 + d), 1e-9f)) good = false;
            good = good && L(stems[0], c48 + kF48) == 0.0f && R(stems[1], c48 + kF48) == kVShort;
        }
        CHECK(good, "render.stems_match_mix");
    }

    // 44.1 kHz project: fade is 220 frames (native-rate assets at 44100 Hz).
    {
        json pj = base_project(1, 1);
        pj["sample_rate"] = 44100;
        pj["tracks"][0]["voice"]["path"] = "long44.wav";
        pj["tracks"][1]["voice"]["path"] = "short44.wav";
        const long long c44 = static_cast<long long>(std::llround(0.125 * (44100.0 * 60.0 / 120.0)));
        op::wav::StereoBuffer out;
        if (render_json(pj, out, err)) {
            bool good = c44 == 2756 && L(out, c44 - 1) == kVLong;
            for (long long d = 0; d < 220; ++d)
                if (!nearf(L(out, c44 + d), expect_fade(kVLong, 220, d), 1e-9f)) good = false;
            good = good && L(out, c44 + 220) == 0.0f;
            CHECK(good, "render.fade_220_frames_at_44k");
        } else {
            CHECK(false, "render.fade_220_frames_at_44k");
        }
    }
}

// ---- set_choke_group op, History undo, diff_projects ----

void test_op_and_diff() {
    std::string err;

    // Set on a sampler track.
    json pj = base_project(1, 1);
    const fs::path opj = g_dir / "op.opj";
    write_file(opj, pj.dump(2) + "\n");
    op::Project p;
    bool loaded = op::load_project(opj.string(), p, err);
    CHECK(loaded, "op.load_base");
    if (!loaded) return;
    const std::string bytes0 = op::project_bytes(p);

    // Project where only track 1 is grouped: its bytes are what clearing track 0 must restore.
    write_file(opj, base_project(0, 1).dump(2) + "\n");
    op::Project plain_p;
    bool ok = loaded && op::load_project(opj.string(), plain_p, err);
    const std::string bytes_plain = op::project_bytes(plain_p);

    // set group 4 on track 0.
    {
        op::Project q = plain_p;
        const json o = json{{"op", "set_choke_group"}, {"index", 0}, {"group", 4}};
        ok = ok && op::apply_op(q, o, err) && op::project_bytes(q).find("\"choke_group\": 4") != std::string::npos;
        // clear with null → byte-identical to the never-grouped project.
        const json clear = json{{"op", "set_choke_group"}, {"index", 0}, {"group", nullptr}};
        ok = ok && op::apply_op(q, clear, err) && op::project_bytes(q) == bytes_plain;
        CHECK(ok, "op.sets_and_clears_byte_identical");
    }

    // Rejections with the exact spec'd messages (prefix "set_choke_group: ").
    {
        bool all = true;
        const json bads = {
            json{{"op", "set_choke_group"}, {"index", 0}, {"group", 4}, {"x", 1}},   // unknown key first
            json{{"op", "set_choke_group"}, {"index", 0}},                            // missing group
            json{{"op", "set_choke_group"}, {"index", 0}, {"group", 1.5}},            // not an integer
            json{{"op", "set_choke_group"}, {"index", 0}, {"group", "4"}},            // not an integer
            json{{"op", "set_choke_group"}, {"index", 0}, {"group", 0}},              // model range
            json{{"op", "set_choke_group"}, {"index", 0}, {"group", 17}},             // model range
        };
        const char* wants[] = {"set_choke_group: unknown argument 'x'",
                               "set_choke_group: missing argument 'group'",
                               "set_choke_group: 'group' must be an integer or null",
                               "set_choke_group: 'group' must be an integer or null",
                               "set_choke_group: track.choke_group must be an integer in [1, 16]",
                               "set_choke_group: track.choke_group must be an integer in [1, 16]"};
        for (size_t i = 0; i < bads.size(); ++i) {
            op::Project q = p;
            std::string e;
            if (op::apply_op(q, bads[i], e) || e != wants[i]) all = false;
            if (op::project_bytes(q) != bytes0) all = false;  // p must be untouched on failure
        }
        CHECK(all, "op.rejects_exact_messages");
    }

    // Non-sampler track → "track is not a sampler voice", also for group null.
    {
        json pj2 = base_project(1, 0);  // only track 0 grouped; track 1 becomes a clean sine track
        json v = json::object();
        v["kind"] = "sine";
        pj2["tracks"][1]["voice"] = v;
        write_file(opj, pj2.dump(2) + "\n");
        op::Project q;
        bool good = op::load_project(opj.string(), q, err);
        const std::string bytes_q = op::project_bytes(q);
        std::string e1, e2;
        good = good && !op::apply_op(q, json{{"op", "set_choke_group"}, {"index", 1}, {"group", 3}}, e1) &&
               e1 == "set_choke_group: track is not a sampler voice" &&
               !op::apply_op(q, json{{"op", "set_choke_group"}, {"index", 1}, {"group", nullptr}}, e2) &&
               e2 == "set_choke_group: track is not a sampler voice" && op::project_bytes(q) == bytes_q;
        CHECK(good, "op.non_sampler_exact_message");
    }

    // History undo restores the exact original bytes.
    {
        op::Project q = plain_p;
        op::History h;
        bool good = h.apply(q, json{{"op", "set_choke_group"}, {"index", 0}, {"group", 9}}, err) &&
                    op::project_bytes(q) != bytes_plain && h.undo(q) && op::project_bytes(q) == bytes_plain;
        CHECK(good, "op.history_undo_byte_exact");
    }

    // diff: adding a group emits exactly one set_choke_group with b's value; removing one emits
    // "group": null; applying the journal reproduces the target bytes in both directions.
    {
        op::Project a = plain_p;  // track 1 grouped (from base_project(0, 1))
        bool good = op::apply_op(a, json{{"op", "set_choke_group"}, {"index", 1}, {"group", nullptr}}, err);
        // Now a has no groups at all: b keeps track 0 grouped → the journal must ADD group 7.
        op::Project b2 = a;
        good = good && op::apply_op(b2, json{{"op", "set_choke_group"}, {"index", 0}, {"group", 7}}, err);
        std::vector<json> ops;
        good = good && op::diff_projects(a, b2, ops, err) && ops.size() == 1 &&
               ops[0].dump() == "{\"group\":7,\"index\":0,\"op\":\"set_choke_group\"}";
        // And the reverse direction emits group null.
        std::vector<json> ops_rev;
        good = good && op::diff_projects(b2, a, ops_rev, err) && ops_rev.size() == 1 &&
               ops_rev[0].dump() == "{\"group\":null,\"index\":0,\"op\":\"set_choke_group\"}";
        std::string journal;
        for (const json& o : ops_rev) journal += o.dump() + "\n";
        op::JournalOutcome oc;
        op::Project work = b2;
        good = good && op::apply_journal(work, journal, err, &oc) && op::project_bytes(work) == op::project_bytes(a);
        CHECK(good, "diff.emits_null_when_removed_and_reproduces");
    }
}

}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / ("op_choke_" + std::to_string(static_cast<long>(::getpid())));
    std::error_code ec;
    fs::remove_all(g_dir, ec);
    fs::create_directories(g_dir, ec);
    if (ec) {
        std::printf("FATAL: cannot create temp dir %s\n", g_dir.string().c_str());
        return 2;
    }

    write_const_asset(g_dir / "long.wav", kFs, kVLong, 9600);
    write_const_asset(g_dir / "short.wav", kFs, kVShort, 4800);
    write_const_asset(g_dir / "long44.wav", 44100, kVLong, 8820);
    write_const_asset(g_dir / "short44.wav", 44100, kVShort, 4410);

    test_validation();
    test_render();
    test_op_and_diff();

    fs::remove_all(g_dir, ec);
    if (g_failures == 0) std::printf("ALL PASS\n");
    return g_failures;
}
