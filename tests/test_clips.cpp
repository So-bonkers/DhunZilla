// Unit tests for op::clips_json (WP-0199, docs/spec/operations.md §Arrangement clip view): the read-only
// arrangement clip view — one entry per clip in model order, stored clip fields plus start/end in beats
// and samples from the renderer's own tempo map. Boring assert-based runner — no framework deps, own main.
// Exit code = number of failures. No file I/O: every Project is built in code.
// Run: ./build/op_clips_tests
#include <cstdio>
#include <string>

#include "engine/model.hpp"
#include "engine/render.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

// 120 bpm, 4/4, 48 kHz → spb = 48000 * 60 / 120 = 24000 samples per beat. One track, no arrangement;
// each test adds the patterns/clips it needs. length_bars is irrelevant to this view (unclamped).
static op::Project base_project() {
    op::Project p;
    p.format_version = 0;
    p.bpm = 120.0;
    p.sample_rate = 48000;
    p.seed = 42;
    p.length_bars = 8.0;
    p.beats_per_bar = 4;
    op::Track t;
    t.name = "lead";
    p.tracks.push_back(t);
    return p;
}

// A pattern on track 0 spanning `span_beats` (one note from beat 0), optionally named.
static void add_span_pattern(op::Project& p, const char* id, double span_beats) {
    op::Pattern pat;
    pat.track = 0;
    pat.id = id;
    op::Note n;
    n.t_beats = 0.0;
    n.dur_beats = span_beats;
    pat.notes.push_back(n);
    p.patterns.push_back(pat);
}

// The spec's worked example: pattern "Intro" spanning 8 beats, clip at bar 1.5, repeats 2, gain −6.5,
// fades 0.5/1.0 → start_beats 6.0 (1.5 × 4), end_beats 22.0 (6 + 2 × 8), samples ×24000.
static op::Project example_project() {
    op::Project p = base_project();
    add_span_pattern(p, "Intro", 8.0);
    op::Clip c;
    c.pattern = 0;
    c.start_bar = 1.5;
    c.repeats = 2;
    c.gain_db = -6.5;
    c.fade_in_beats = 0.5;
    c.fade_out_beats = 1.0;
    p.arrangement.push_back(c);
    return p;
}

int main() {
    // --- empty / absent arrangement → exactly "[]\n" (implicit legacy placements are NOT synthesized).
    {
        op::Project p = base_project();
        add_span_pattern(p, "A", 4.0);
        add_span_pattern(p, "B", 4.0);
        add_span_pattern(p, "C", 2.0);  // three patterns, zero clips → [] and nothing invented
        const std::string s = op::clips_json(p);
        CHECK(s == "[]\n", "clips.empty_arrangement_prints_empty_array_even_with_patterns");
    }

    // --- byte-exact canonical entry (the spec example).
    {
        const std::string want = R"JSON([
  {
    "end_beats": 22.0,
    "end_sample": 528000,
    "fade_in_beats": 0.5,
    "fade_out_beats": 1.0,
    "gain_db": -6.5,
    "index": 0,
    "pattern": 0,
    "pattern_name": "Intro",
    "repeats": 2,
    "start_bar": 1.5,
    "start_beats": 6.0,
    "start_sample": 144000
  }
]
)JSON";
        const std::string s = op::clips_json(example_project());
        CHECK(s == want, "clips.canonical_entry_byte_exact");
        CHECK(s.size() >= 3 && s[s.size() - 1] == '\n' && s[s.size() - 2] != '\n',
              "clips.exactly_one_trailing_newline");
    }

    // --- unnamed pattern → the pattern_name key is omitted entirely (never "" / null / "Pattern 0").
    {
        op::Project p = example_project();
        p.patterns[0].id.clear();
        const std::string s = op::clips_json(p);
        CHECK(s.find("pattern_name") == std::string::npos, "clips.unnamed_pattern_omits_pattern_name_key");
        CHECK(s.find("\"pattern\": 0") != std::string::npos &&
                  s.find("\"start_sample\": 144000") != std::string::npos,
              "clips.unnamed_pattern_still_reports_index_and_geometry");
    }

    // --- model order preserved when the clips are out of bar order (never sorted by position).
    {
        op::Project p = base_project();
        add_span_pattern(p, "Intro", 8.0);
        op::Clip late;  // index 0 but at bar 4.0
        late.pattern = 0;
        late.start_bar = 4.0;
        late.repeats = 1;
        op::Clip early;  // index 1 but at bar 1.0
        early.pattern = 0;
        early.start_bar = 1.0;
        early.repeats = 1;
        p.arrangement.push_back(late);
        p.arrangement.push_back(early);
        const std::string s = op::clips_json(p);
        CHECK(s.find("\"start_bar\": 4.0") < s.find("\"start_bar\": 1.0"), "clips.model_order_not_sorted_by_bar");
        CHECK(s.find("\"index\": 0") < s.find("\"index\": 1"), "clips.index_keys_follow_model_order");
    }

    // --- an explicit pattern length_beats is the loop period, not the note span.
    {
        op::Project p = base_project();
        add_span_pattern(p, "Loop", 8.0);
        p.patterns[0].length_beats = 3.0;  // extent 3.0 → end_beats 0 + 2 × 3 = 6.0 → 144000 samples
        op::Clip c;
        c.pattern = 0;
        c.start_bar = 0.0;
        c.repeats = 2;
        p.arrangement.push_back(c);
        const std::string s = op::clips_json(p);
        CHECK(s.find("\"end_beats\": 6.0") != std::string::npos &&
                  s.find("\"end_sample\": 144000") != std::string::npos,
              "clips.explicit_length_beats_is_the_loop_period");
    }

    // --- empty pattern (no notes, no length_beats): extent 0 → end == start for ANY repeats, and the
    //     stored repeats is still reported verbatim.
    {
        op::Project p = base_project();
        op::Pattern pat;  // no notes, no length_beats → pattern_extent 0.0
        pat.track = 0;
        pat.id = "Empty";
        p.patterns.push_back(pat);
        op::Clip c;
        c.pattern = 0;
        c.start_bar = 2.0;  // start_beats 8.0 → 192000 samples; extent 0 → end identical
        c.repeats = 7;
        p.arrangement.push_back(c);
        const std::string s = op::clips_json(p);
        CHECK(s.find("\"end_beats\": 8.0") != std::string::npos &&
                  s.find("\"end_sample\": 192000") != std::string::npos,
              "clips.empty_pattern_end_equals_start");
        CHECK(s.find("\"repeats\": 7") != std::string::npos, "clips.empty_pattern_reports_stored_repeats");
    }

    // --- defaults are always present and print as 0.0 (integral-valued doubles keep the ".0").
    {
        op::Project p = base_project();
        add_span_pattern(p, "Intro", 4.0);
        p.arrangement.push_back(op::Clip{});  // gain_db / fades all default to 0.0
        const std::string s = op::clips_json(p);
        CHECK(s.find("\"gain_db\": 0.0") != std::string::npos &&
                  s.find("\"fade_in_beats\": 0.0") != std::string::npos &&
                  s.find("\"fade_out_beats\": 0.0") != std::string::npos,
              "clips.defaults_print_as_zero_point_zero");
    }

    // --- clip trim keys (WP-0193a) are NOT part of this view and do NOT affect the geometry.
    {
        op::Project plain = base_project();
        add_span_pattern(plain, "Intro", 8.0);
        op::Clip c;
        c.pattern = 0;
        c.start_bar = 1.5;
        c.repeats = 2;
        plain.arrangement.push_back(c);

        op::Project trimmed = plain;
        trimmed.arrangement[0].offset_beats = 1.5;
        trimmed.arrangement[0].length_beats = 2.25;

        const std::string a = op::clips_json(plain);
        const std::string b = op::clips_json(trimmed);
        CHECK(a == b, "clips.trim_keys_do_not_change_output");
        CHECK(b.find("offset_beats") == std::string::npos && b.find("length_beats") == std::string::npos,
              "clips.trim_keys_absent_from_output");
    }

    // --- tempo map (120 bpm from bar 0, 60 bpm from bar 2): samples accumulate per segment with llround.
    // Segment 0: beats [0, 8) at spb 24000 → beat 8 lands on sample 192000.
    // Segment 1: beats [8, …) at spb 48000 → beat 12 = 192000 + 4 × 48000 = 384000.
    {
        op::Project p = base_project();
        p.tempo_map.push_back(op::TempoPoint{0.0, 120.0});
        p.tempo_map.push_back(op::TempoPoint{2.0, 60.0});
        add_span_pattern(p, "Intro", 4.0);  // extent 4.0
        op::Clip before_bp;                 // clip entirely inside segment 0
        before_bp.pattern = 0;
        before_bp.start_bar = 1.0;  // beats [4, 8) → samples [96000, 192000)
        before_bp.repeats = 1;
        op::Clip on_bp;  // clip starting exactly ON the breakpoint
        on_bp.pattern = 0;
        on_bp.start_bar = 2.0;  // beats [8, 12) → samples [192000, 384000)
        on_bp.repeats = 1;
        p.arrangement.push_back(before_bp);
        p.arrangement.push_back(on_bp);

        const std::string want = R"JSON([
  {
    "end_beats": 8.0,
    "end_sample": 192000,
    "fade_in_beats": 0.0,
    "fade_out_beats": 0.0,
    "gain_db": 0.0,
    "index": 0,
    "pattern": 0,
    "pattern_name": "Intro",
    "repeats": 1,
    "start_bar": 1.0,
    "start_beats": 4.0,
    "start_sample": 96000
  },
  {
    "end_beats": 12.0,
    "end_sample": 384000,
    "fade_in_beats": 0.0,
    "fade_out_beats": 0.0,
    "gain_db": 0.0,
    "index": 1,
    "pattern": 0,
    "pattern_name": "Intro",
    "repeats": 1,
    "start_bar": 2.0,
    "start_beats": 8.0,
    "start_sample": 192000
  }
]
)JSON";
        const std::string s = op::clips_json(p);
        CHECK(s == want, "clips.tempo_map_piecewise_byte_exact");

        // The breakpoint clip: start maps through the OLD segment end (192000) and its length at 60 bpm.
        const long long total = op::total_samples_for(p);  // length_bars 8 → beats 32 → 192000 + 24 × 48000
        CHECK(total == 192000 + 24 * 48000, "clips.tempo_map_total_samples_sanity");

        // Same project run twice → byte-identical (pure function of the model).
        CHECK(op::clips_json(p) == s, "clips.repeat_calls_byte_identical");
    }

    // --- unclamped: a clip may extend past length_bars and reports its true mapped end.
    {
        op::Project p = base_project();
        p.length_bars = 2.0;  // total = ceil(8 beats × 24000) = 192000 samples
        add_span_pattern(p, "Intro", 8.0);
        op::Clip c;
        c.pattern = 0;
        c.start_bar = 1.5;   // start_beats 6.0 → 144000
        c.repeats = 4;       // end_beats 6 + 32 = 38.0 → 912000, far past length_bars
        p.arrangement.push_back(c);
        const std::string s = op::clips_json(p);
        CHECK(s.find("\"end_sample\": 912000") != std::string::npos &&
                  s.find("\"end_beats\": 38.0") != std::string::npos,
              "clips.unclamped_end_past_length_bars");
        CHECK(op::total_samples_for(p) == 192000 && op::total_samples_for(p) < 912000,
              "clips.end_sample_may_exceed_total_samples_for");
    }

    // --- fractional start_bar at a non-120 bpm: 96 bpm → spb = 48000 × 60 / 96 = 30000.
    {
        op::Project p = base_project();
        p.bpm = 96.0;
        add_span_pattern(p, "Groove", 2.5);  // extent 2.5
        op::Clip c;
        c.pattern = 0;
        c.start_bar = 1.25;  // start_beats 5.0 → 150000 samples
        c.repeats = 2;       // end_beats 5 + 5 = 10.0 → 300000 samples
        p.arrangement.push_back(c);
        const std::string want = R"JSON([
  {
    "end_beats": 10.0,
    "end_sample": 300000,
    "fade_in_beats": 0.0,
    "fade_out_beats": 0.0,
    "gain_db": 0.0,
    "index": 0,
    "pattern": 0,
    "pattern_name": "Groove",
    "repeats": 2,
    "start_bar": 1.25,
    "start_beats": 5.0,
    "start_sample": 150000
  }
]
)JSON";
        CHECK(op::clips_json(p) == want, "clips.fractional_start_bar_non_120_bpm");
    }

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
