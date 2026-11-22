// Unit tests for the WP-0203 additive lint rules 12-14 (docs/spec/lint.md §"Additive rules for
// recently shipped features (WP-0203)"). Boring assert-based runner — no framework deps.
// Exit code = number of failures. Run: ./build/op_lint_rules_tests
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/lint.hpp"
#include "engine/model.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

// Returns the first issue with this code, or nullptr.
static const op::lint::Issue* find(const std::vector<op::lint::Issue>& v, const std::string& code) {
    for (const auto& is : v)
        if (is.code == code) return &is;
    return nullptr;
}

static int count_code(const std::vector<op::lint::Issue>& v, const std::string& code) {
    int n = 0;
    for (const auto& is : v)
        if (is.code == code) ++n;
    return n;
}

// A project that must produce zero issues: one sine track, one pattern whose single note gives an
// extent of exactly 1.0 beat, one clip at bar 0 with no fades (same shape as tests/test_lint.cpp).
static op::Project clean_project() {
    op::Project p;  // defaults: bpm 120, length_bars 4, beats_per_bar 4
    op::Track t;
    t.name = "A";
    t.voice.kind = op::VoiceKind::Sine;
    p.tracks = {t};
    op::Pattern pat;
    pat.track = 0;
    pat.notes.push_back({0.0, 1.0});
    p.patterns = {pat};
    op::Clip c;
    c.pattern = 0;
    c.start_bar = 0.0;
    c.repeats = 1;
    p.arrangement = {c};
    return p;
}

// ---------------------------------------------------------------- rule 12: fades vs clip span

static void test_rule12_flagged_exact() {
    auto p = clean_project();  // extent 1.0, repeats 1 → span 1.0
    p.arrangement[0].fade_in_beats = 1.0;
    p.arrangement[0].fade_out_beats = 1.0;  // sum 2.0 > 1.0 + 1e-9 → flagged
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "fades_exceed_clip_length");
    CHECK(is != nullptr && count_code(issues, "fades_exceed_clip_length") == 1,
          "rule12_flagged_exactly_once");
    CHECK(is && is->severity == "warning" && is->path == "arrangement[0]" &&
              is->message == "fade_in_beats + fade_out_beats exceed the clip length",
          "rule12_severity_path_message_exact");
}

static void test_rule12_sum_equals_span_not_flagged() {
    auto p = clean_project();  // span 1.0
    p.arrangement[0].fade_in_beats = 0.5;
    p.arrangement[0].fade_out_beats = 0.5;  // sum == span exactly → NOT flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 0,
          "rule12_sum_equals_span_not_flagged");
}

static void test_rule12_sum_below_span_not_flagged() {
    auto p = clean_project();
    p.arrangement[0].fade_in_beats = 0.4;
    p.arrangement[0].fade_out_beats = 0.5;  // sum 0.9 < span 1.0 → NOT flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 0,
          "rule12_sum_below_span_not_flagged");
}

static void test_rule12_repeats_widen_span() {
    auto p = clean_project();  // extent 1.0
    p.arrangement[0].repeats = 4;
    p.arrangement[0].fade_in_beats = 2.0;
    p.arrangement[0].fade_out_beats = 2.0;  // sum 4.0 == span 4*1.0 → fits, NOT flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 0,
          "rule12_repeats_fit");
    p.arrangement[0].fade_in_beats = 2.5;  // sum 4.5 > span 4.0 → flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 1,
          "rule12_repeats_overshoot");
}

static void test_rule12_honours_length_beats() {
    auto p = clean_project();  // note extent 1.0
    p.patterns[0].length_beats = 4.0;  // explicit extent wins → span 4.0
    p.arrangement[0].fade_in_beats = 2.0;
    p.arrangement[0].fade_out_beats = 2.0;  // sum 4.0 == span → NOT flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 0,
          "rule12_length_beats_equality_not_flagged");
    p.patterns[0].length_beats.reset();  // back to the note extent 1.0 → sum 4.0 > 1.0 → flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 1,
          "rule12_length_beats_removed_flagged");
}

static void test_rule12_ignores_clip_trim_keys() {
    auto p = clean_project();  // untrimmed span 1.0
    p.arrangement[0].length_beats = 0.25;  // WP-0193a trim: must NOT shorten the linted span
    p.arrangement[0].fade_in_beats = 0.5;
    p.arrangement[0].fade_out_beats = 0.5;  // sum == untrimmed span → NOT flagged (trim ignored)
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 0,
          "rule12_clip_trim_keys_ignored");
}

static void test_rule12_zero_fades_never_flagged() {
    auto p = clean_project();
    p.arrangement[0].repeats = 3;  // any span, both fades zero → never flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 0,
          "rule12_zero_fades_not_flagged");
}

static void test_rule12_empty_pattern_extent_zero() {
    auto p = clean_project();
    p.patterns[0].notes.clear();  // no notes, no length_beats → extent 0, span 0
    p.arrangement[0].fade_in_beats = 0.5;  // any positive sum > 0 + 1e-9 → flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 1,
          "rule12_empty_pattern_positive_fade_flagged");
}

static void test_rule12_out_of_range_pattern_extent_zero() {
    auto p = clean_project();
    p.arrangement[0].pattern = 99;  // hand-built struct: impossible from a parsed file → extent 0.0
    p.arrangement[0].fade_in_beats = 0.5;
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 1,
          "rule12_out_of_range_pattern_flagged");
}

static void test_rule12_two_clips_only_one_offends() {
    auto p = clean_project();
    op::Clip c2;  // same pattern, later start_bar (no rule-4 duplicate), zero fades
    c2.pattern = 0;
    c2.start_bar = 1.0;
    c2.repeats = 1;
    p.arrangement.push_back(c2);
    p.arrangement[1].fade_in_beats = 2.0;  // only clip 1 overshoots its span of 1.0
    const auto issues = op::lint::lint_project(p, "");
    CHECK(count_code(issues, "fades_exceed_clip_length") == 1, "rule12_two_clips_exactly_one_issue");
    const op::lint::Issue* is = find(issues, "fades_exceed_clip_length");
    CHECK(is && is->path == "arrangement[1]", "rule12_path_names_offending_clip");
}

static void test_rule12_rounding_inside_guard() {
    auto p = clean_project();
    p.patterns[0].notes[0] = {0.0, 0.3};  // extent 0.3
    p.arrangement[0].fade_in_beats = 0.1;
    p.arrangement[0].fade_out_beats = 0.2;  // binary 0.1+0.2 exceeds 0.3 by ~5.6e-17 → inside 1e-9
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 0,
          "rule12_rounding_inside_guard_not_flagged");
}

static void test_rule12_genuine_overshoot_reported() {
    auto p = clean_project();
    p.patterns[0].notes[0] = {0.0, 0.5};  // extent 0.5
    p.arrangement[0].fade_in_beats = 0.5001;  // overshoot ~1e-4 beats ≫ 1e-9 → IS reported
    CHECK(count_code(op::lint::lint_project(p, ""), "fades_exceed_clip_length") == 1,
          "rule12_overshoot_1e_minus_4_flagged");
}

// ---------------------------------------------------------------- rule 13: lone choke group

static void test_rule13_lone_group_flagged_exact() {
    auto p = clean_project();
    p.tracks[0].choke_group = 3;  // sole member of group 3 → flagged
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "choke_group_single_track");
    CHECK(is != nullptr && count_code(issues, "choke_group_single_track") == 1,
          "rule13_lone_group_exactly_once");
    CHECK(is && is->severity == "info" && is->path == "tracks[0]" &&
              is->message == "choke group has only one track",
          "rule13_severity_path_message_exact");
}

static void test_rule13_matching_pair_not_flagged() {
    auto p = clean_project();
    op::Track t2;  // second track with its own pattern (rules 1/3 stay quiet) and the same group
    t2.name = "B";
    t2.voice.kind = op::VoiceKind::Sine;
    t2.choke_group = 3;
    p.tracks.push_back(t2);
    op::Pattern pat2;
    pat2.track = 1;
    pat2.notes.push_back({0.0, 1.0});
    p.patterns.push_back(pat2);
    op::Clip c2;
    c2.pattern = 1;
    c2.start_bar = 1.0;
    c2.repeats = 1;
    p.arrangement.push_back(c2);
    p.tracks[0].choke_group = 3;
    CHECK(count_code(op::lint::lint_project(p, ""), "choke_group_single_track") == 0,
          "rule13_pair_not_flagged");
}

static void test_rule13_two_lone_groups_both_flagged() {
    auto p = clean_project();
    op::Track t2;
    t2.name = "B";
    t2.voice.kind = op::VoiceKind::Sine;
    t2.choke_group = 2;
    p.tracks.push_back(t2);
    op::Pattern pat2;
    pat2.track = 1;
    pat2.notes.push_back({0.0, 1.0});
    p.patterns.push_back(pat2);
    op::Clip c2;
    c2.pattern = 1;
    c2.start_bar = 1.0;
    c2.repeats = 1;
    p.arrangement.push_back(c2);
    p.tracks[0].choke_group = 1;  // groups 1 and 2, one member each → both flagged
    const auto issues = op::lint::lint_project(p, "");
    CHECK(count_code(issues, "choke_group_single_track") == 2, "rule13_two_lone_groups_both_flagged");
    int seen = 0;
    for (const auto& is : issues)
        if (is.code == "choke_group_single_track" && is.path == "tracks[" + std::to_string(seen) + "]") ++seen;
    CHECK(seen == 2, "rule13_two_lone_groups_paths_ascending");
}

static void test_rule13_zero_group_never_flagged() {
    auto p = clean_project();  // default choke_group 0 on every track → never flagged
    op::Track t2;
    t2.name = "B";
    t2.voice.kind = op::VoiceKind::Sine;
    p.tracks.push_back(t2);
    CHECK(count_code(op::lint::lint_project(p, ""), "choke_group_single_track") == 0,
          "rule13_zero_group_never_flagged");
}

// ---------------------------------------------------------------- rule 14: dead swing

static void test_rule14_swing_off_not_flagged() {
    auto p = clean_project();  // swing default 0.0 → never flagged even with only even-step notes
    CHECK(count_code(op::lint::lint_project(p, ""), "swing_without_odd_sixteenths") == 0,
          "rule14_swing_zero_not_flagged");
    p.patterns[0].swing = -0.0;  // -0.0 == 0.0: a saved -0.0 means off → NOT flagged
    CHECK(count_code(op::lint::lint_project(p, ""), "swing_without_odd_sixteenths") == 0,
          "rule14_swing_negative_zero_not_flagged");
}

static void test_rule14_even_steps_only_flagged_exact() {
    auto p = clean_project();
    p.patterns[0].swing = 0.5;
    p.patterns[0].notes.push_back({0.5, 0.25});  // t = 0.0 and 0.5: even 16ths only → flagged
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "swing_without_odd_sixteenths");
    CHECK(is != nullptr && count_code(issues, "swing_without_odd_sixteenths") == 1,
          "rule14_even_steps_exactly_once");
    CHECK(is && is->severity == "info" && is->path == "patterns[0]" &&
              is->message == "swing is set but no note starts on an odd 16th",
          "rule14_severity_path_message_exact");
}

static void test_rule14_odd_sixteenth_note_clears_it() {
    auto p = clean_project();
    p.patterns[0].swing = 0.5;
    p.patterns[0].notes.push_back({0.75, 0.25});  // t = 0.75: q = 3 → odd 16th, swing moves it
    CHECK(count_code(op::lint::lint_project(p, ""), "swing_without_odd_sixteenths") == 0,
          "rule14_note_at_0_75_not_flagged");
}

static void test_rule14_non_quarter_step_still_flagged() {
    auto p = clean_project();
    p.patterns[0].swing = 0.5;
    p.patterns[0].notes[0] = {0.3, 0.2};  // t = 0.3: q = 1.2 is not an integer → swing never moves it
    CHECK(count_code(op::lint::lint_project(p, ""), "swing_without_odd_sixteenths") == 1,
          "rule14_note_at_0_3_flagged");
}

static void test_rule14_no_notes_with_swing_flagged() {
    auto p = clean_project();
    p.patterns[0].notes.clear();  // no notes at all → nothing swing could move → flagged
    p.patterns[0].swing = 0.25;
    CHECK(count_code(op::lint::lint_project(p, ""), "swing_without_odd_sixteenths") == 1,
          "rule14_no_notes_with_swing_flagged");
}

// ---------------------------------------------------------------- ordering and exact JSON

static void test_new_rules_after_rule5_on_same_clip() {
    auto p = clean_project();
    p.arrangement[0].start_bar = 4.0;      // rule 5 clip_past_end on arrangement[0] ...
    p.arrangement[0].fade_in_beats = 2.0;  // ... and rule 12 on the same clip, after it
    const auto issues = op::lint::lint_project(p, "");
    int i5 = -1, i12 = -1;
    for (int i = 0; i < static_cast<int>(issues.size()); ++i) {
        if (issues[i].code == "clip_past_end" && i5 < 0) i5 = i;
        if (issues[i].code == "fades_exceed_clip_length" && i12 < 0) i12 = i;
    }
    CHECK(i5 >= 0 && i12 >= 0 && i5 < i12, "rule12_after_existing_rule_on_same_clip");
}

static void test_exact_json_three_new_rules_in_order() {
    // Track 1 is a sampler on /dev/null so rule 8 stays quiet; its pattern + clip keep rules 1/3 quiet.
    auto p = clean_project();
    op::Track t2;
    t2.name = "B";
    t2.voice.kind = op::VoiceKind::Sampler;
    t2.voice.path = "/dev/null";
    t2.choke_group = 3;  // lone group → rule 13 on tracks[1]
    p.tracks.push_back(t2);
    p.patterns[0].swing = 0.5;  // notes at t = 0.0 only → rule 14 on patterns[0]
    op::Pattern pat2;
    pat2.track = 1;
    pat2.notes.push_back({0.0, 1.0});
    p.patterns.push_back(pat2);
    op::Clip c2;
    c2.pattern = 1;
    c2.start_bar = 1.0;
    c2.repeats = 1;
    p.arrangement.push_back(c2);
    p.arrangement[0].fade_in_beats = 1.0;   // sum 2.0 > span 1.0 → rule 12 on arrangement[0]
    p.arrangement[0].fade_out_beats = 1.0;
    const std::string json = op::lint::to_json(op::lint::lint_project(p, ""));
    CHECK(json == "{\"version\":1,\"errors\":0,\"warnings\":1,\"infos\":2,\"issues\":["
                  "{\"severity\":\"warning\",\"code\":\"fades_exceed_clip_length\",\"path\":\"arrangement[0]\","
                  "\"message\":\"fade_in_beats + fade_out_beats exceed the clip length\"},"
                  "{\"severity\":\"info\",\"code\":\"choke_group_single_track\",\"path\":\"tracks[1]\","
                  "\"message\":\"choke group has only one track\"},"
                  "{\"severity\":\"info\",\"code\":\"swing_without_odd_sixteenths\",\"path\":\"patterns[0]\","
                  "\"message\":\"swing is set but no note starts on an odd 16th\"}]}\n",
          "exact_json_rules_12_13_14_in_order");
}

static void test_clean_project_still_zero_issues() {
    // Regression net: the three new loops must not fire on the pre-WP-0203 clean baseline.
    CHECK(op::lint::lint_project(clean_project(), "").empty(), "clean_project_no_new_issues");
}

int main() {
    test_rule12_flagged_exact();
    test_rule12_sum_equals_span_not_flagged();
    test_rule12_sum_below_span_not_flagged();
    test_rule12_repeats_widen_span();
    test_rule12_honours_length_beats();
    test_rule12_ignores_clip_trim_keys();
    test_rule12_zero_fades_never_flagged();
    test_rule12_empty_pattern_extent_zero();
    test_rule12_out_of_range_pattern_extent_zero();
    test_rule12_two_clips_only_one_offends();
    test_rule12_rounding_inside_guard();
    test_rule12_genuine_overshoot_reported();
    test_rule13_lone_group_flagged_exact();
    test_rule13_matching_pair_not_flagged();
    test_rule13_two_lone_groups_both_flagged();
    test_rule13_zero_group_never_flagged();
    test_rule14_swing_off_not_flagged();
    test_rule14_even_steps_only_flagged_exact();
    test_rule14_odd_sixteenth_note_clears_it();
    test_rule14_non_quarter_step_still_flagged();
    test_rule14_no_notes_with_swing_flagged();
    test_new_rules_after_rule5_on_same_clip();
    test_exact_json_three_new_rules_in_order();
    test_clean_project_still_zero_issues();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}
