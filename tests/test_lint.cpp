// Unit tests for op::lint (docs/spec/lint.md, WP-0170). Boring assert-based runner — no framework deps.
// Exit code = number of failures. Run: ./build/op_lint_tests
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "engine/lint.hpp"
#include "engine/model.hpp"

namespace fs = std::filesystem;

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

// A project that must produce zero issues: one sine track, one pattern with one note, one clip at bar 0.
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

static void test_clean_project() {
    const auto issues = op::lint::lint_project(clean_project(), "");
    CHECK(issues.empty(), "clean_project_no_issues");
}

static void test_to_json_empty() {
    CHECK(op::lint::to_json({}) == "{\"version\":1,\"errors\":0,\"warnings\":0,\"infos\":0,\"issues\":[]}\n",
          "to_json_empty_exact");
}

static void test_rule1_track_no_notes() {
    auto p = clean_project();
    op::Track t2;  // a second track with no patterns at all
    t2.name = "B";
    t2.voice.kind = op::VoiceKind::Sine;
    p.tracks.push_back(t2);
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "track_no_notes");
    CHECK(is && is->severity == "warning" && is->path == "tracks[1]" && is->message == "track has no notes",
          "rule1_track_no_notes");
}

static void test_rule2_pattern_empty() {
    auto p = clean_project();
    op::Pattern empty;  // pattern with zero notes, still on track 0 (which keeps its note via patterns[0])
    empty.track = 0;
    p.patterns.push_back(empty);
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "pattern_empty");
    CHECK(is && is->severity == "warning" && is->path == "patterns[1]" && is->message == "pattern has no notes",
          "rule2_pattern_empty");
}

static void test_rule3_pattern_unused() {
    auto p = clean_project();
    op::Pattern extra;  // has notes, but no clip references it
    extra.track = 0;
    extra.notes.push_back({0.0, 1.0});
    p.patterns.push_back(extra);
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "pattern_unused");
    CHECK(is && is->severity == "warning" && is->path == "patterns[1]" &&
              is->message == "pattern is not referenced by any clip",
          "rule3_pattern_unused");
}

static void test_rule4_clip_duplicate() {
    auto p = clean_project();
    op::Clip dup;  // same pattern, exactly equal start_bar as arrangement[0]
    dup.pattern = 0;
    dup.start_bar = 0.0;
    dup.repeats = 1;
    p.arrangement.push_back(dup);
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "clip_duplicate");
    CHECK(is && is->severity == "warning" && is->path == "arrangement[1]" &&
              is->message == "duplicate of arrangement[0]",
          "rule4_clip_duplicate");
}

static void test_rule5_clip_past_end() {
    auto p = clean_project();
    p.arrangement[0].start_bar = 4.0;  // == length_bars → at or after the end
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "clip_past_end");
    CHECK(is && is->severity == "warning" && is->path == "arrangement[0]" &&
              is->message == "clip starts at or after length_bars",
          "rule5_clip_past_end");
}

static void test_rule6_clip_overruns_length() {
    auto p = clean_project();  // pattern extent = 1.0 beat, len_beats = 16
    p.arrangement[0].start_bar = 3.75;
    p.arrangement[0].repeats = 2;  // 3.75*4 + 2*1 = 17 > 16 → overrun
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "clip_overruns_length");
    CHECK(is && is->severity == "warning" && is->path == "arrangement[0]" &&
              is->message == "clip ends after length_bars",
          "rule6_clip_overruns_length");
}

static void test_rule6_boundary_exact_end() {
    auto p = clean_project();
    p.patterns[0].notes[0].dur_beats = 4.0;  // extent = 4 beats
    p.arrangement[0].start_bar = 3.0;        // 3*4 + 1*4 = 16 == len_beats → NOT an overrun
    p.arrangement[0].repeats = 1;
    const auto issues = op::lint::lint_project(p, "");
    CHECK(find(issues, "clip_overruns_length") == nullptr, "rule6_exact_end_not_flagged");
}

static void test_rule6_honours_length_beats() {  // WP-0162 integration: explicit length_beats is the extent
    auto p = clean_project();  // notes extent = 1.0 beat, len_beats = 16
    p.patterns[0].length_beats = 4.0;
    p.arrangement[0].start_bar = 3.0;
    p.arrangement[0].repeats = 2;  // 3*4 + 2*4 = 20 > 16 → overrun (would be 14 with the note extent)
    CHECK(find(op::lint::lint_project(p, ""), "clip_overruns_length") != nullptr, "rule6_length_beats_overrun");
    p.patterns[0].length_beats = 0.5;
    p.arrangement[0].start_bar = 3.75;  // 3.75*4 + 2*0.5 = 16 == len_beats → NOT an overrun (17 by notes)
    CHECK(find(op::lint::lint_project(p, ""), "clip_overruns_length") == nullptr, "rule6_length_beats_exact_end");
}

static void test_rule7_notes_past_end() {
    auto p = clean_project();
    p.arrangement.clear();  // legacy semantics: patterns play once from bar 0
    p.patterns[0].notes.push_back({16.0, 1.0});  // len_beats = 16: two notes at or past the end
    p.patterns[0].notes.push_back({20.0, 1.0});
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "notes_past_end");
    CHECK(is && is->severity == "warning" && is->path == "patterns[0]" &&
              is->message == "2 note(s) start at or after length_bars",
          "rule7_notes_past_end");
}

static void test_rule8_sampler_missing() {
    auto p = clean_project();
    p.tracks[0].voice.kind = op::VoiceKind::Sampler;
    p.tracks[0].voice.path = "kits/kick.wav";  // relative → resolved against base_dir, which does not exist
    const auto issues = op::lint::lint_project(p, "/nonexistent-op-0170-dir");
    const op::lint::Issue* is = find(issues, "sampler_file_missing");
    CHECK(is && is->severity == "error" && is->path == "tracks[0].voice.path" &&
              is->message == "sample file not found: kits/kick.wav",
          "rule8_sampler_file_missing");
}

static void test_rule8_sampler_present() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "op_lint_tests_wp0170";
    fs::create_directories(dir, ec);
    if (ec) { CHECK(false, "rule8_present_setup"); return; }
    const fs::path wav = dir / "kick.wav";
    { std::ofstream f(wav, std::ios::binary); f << "RIFF....WAVEfmt "; }
    auto p = clean_project();
    p.tracks[0].voice.kind = op::VoiceKind::Sampler;
    p.tracks[0].voice.path = "kick.wav";  // exists in base_dir → no issue
    const auto issues = op::lint::lint_project(p, dir.string());
    CHECK(find(issues, "sampler_file_missing") == nullptr, "rule8_existing_file_no_issue");
    fs::remove(wav, ec);
    fs::remove(dir, ec);
}

static void test_rule9_track_muted() {
    auto p = clean_project();
    p.tracks[0].mute = true;
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "track_muted");
    CHECK(is && is->severity == "info" && is->path == "tracks[0]" && is->message == "track is muted",
          "rule9_track_muted");
}

static void test_rule10_silenced_by_solo() {
    auto p = clean_project();
    op::Track t2;  // not soloed, not muted → silenced by track 0's solo
    t2.name = "B";
    t2.voice.kind = op::VoiceKind::Sine;
    p.tracks.push_back(t2);
    p.tracks[0].solo = true;
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "track_silenced_by_solo");
    CHECK(is && is->severity == "info" && is->path == "tracks[1]" &&
              is->message == "silenced because another track is soloed",
          "rule10_track_silenced_by_solo");
}

static void test_rule10_muted_not_also_silenced() {
    auto p = clean_project();
    op::Track t2;
    t2.name = "B";
    t2.voice.kind = op::VoiceKind::Sine;
    t2.mute = true;  // muted AND another track soloed: rule 9 only, never rule 10
    p.tracks.push_back(t2);
    p.tracks[0].solo = true;
    const auto issues = op::lint::lint_project(p, "");
    CHECK(count_code(issues, "track_silenced_by_solo") == 0 && count_code(issues, "track_muted") == 1,
          "rule10_muted_track_not_also_reported");
}

static void test_rule11_tempo_point_past_end() {
    auto p = clean_project();
    op::TempoPoint tp;
    tp.bar = 4.0;  // == length_bars → the song never reaches it
    tp.bpm = 90.0;
    p.tempo_map.push_back(tp);
    const auto issues = op::lint::lint_project(p, "");
    const op::lint::Issue* is = find(issues, "tempo_point_past_end");
    CHECK(is && is->severity == "warning" && is->path == "tempo_map[0]" &&
              is->message == "tempo change at or after length_bars",
          "rule11_tempo_point_past_end");
}

static void test_rule_ordering() {
    auto p = clean_project();
    p.patterns[0].notes.clear();  // triggers rule 1 (track has no pattern with notes) AND rule 2
    const auto issues = op::lint::lint_project(p, "");
    int i1 = -1, i2 = -1;
    for (int i = 0; i < static_cast<int>(issues.size()); ++i) {
        if (issues[i].code == "track_no_notes" && i1 < 0) i1 = i;
        if (issues[i].code == "pattern_empty" && i2 < 0) i2 = i;
    }
    CHECK(i1 >= 0 && i2 >= 0 && i1 < i2, "rule_order_rule1_before_rule2");
}

static void test_to_json_one_warning_exact() {
    auto p = clean_project();
    p.arrangement[0].start_bar = 9.0;  // exactly one issue: rule 5 clip_past_end (rule 6 is NOT rule 5)
    const std::string json = op::lint::to_json(op::lint::lint_project(p, ""));
    CHECK(json == "{\"version\":1,\"errors\":0,\"warnings\":1,\"infos\":0,\"issues\":[{"
                  "\"severity\":\"warning\",\"code\":\"clip_past_end\",\"path\":\"arrangement[0]\","
                  "\"message\":\"clip starts at or after length_bars\"}]}\n",
          "to_json_one_warning_exact");
}

int main() {
    test_clean_project();
    test_to_json_empty();
    test_rule1_track_no_notes();
    test_rule2_pattern_empty();
    test_rule3_pattern_unused();
    test_rule4_clip_duplicate();
    test_rule5_clip_past_end();
    test_rule6_clip_overruns_length();
    test_rule6_boundary_exact_end();
    test_rule6_honours_length_beats();
    test_rule7_notes_past_end();
    test_rule8_sampler_missing();
    test_rule8_sampler_present();
    test_rule9_track_muted();
    test_rule10_silenced_by_solo();
    test_rule10_muted_not_also_silenced();
    test_rule11_tempo_point_past_end();
    test_rule_ordering();
    test_to_json_one_warning_exact();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures;
}
