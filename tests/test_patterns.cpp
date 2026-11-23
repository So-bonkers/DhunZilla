// Unit tests for WP-0228 (`opd patterns`): op::patterns_json — the read-only pattern-bar listing as
// canonical JSON (model order, effective length via the renderer's own op::pattern_extent, swing, note
// counts, using clip indices). Boring assert-based runner — no framework deps, own main. Exit code =
// number of failures. All project files are written into one per-process scratch directory under
// temp_directory_path() (pid in the name) and removed at the end; nothing outside it is created.
// Runs the opd binary via OPD_BIN.
// Run: ./build/op_patterns_tests
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/model.hpp"
#include "engine/patterns.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;

// ---------------------------------------------------------------- scratch directory + file helpers

static const fs::path& scratch_dir() {
    static const fs::path dir = [] {
        const fs::path p = fs::temp_directory_path() /
                           ("op_patterns_tests_wp0228_" + std::to_string(static_cast<long>(::getpid())));
        std::error_code ec;
        fs::remove_all(p, ec);
        fs::create_directories(p, ec);
        return p;
    }();
    return dir;
}

static void write_file(const fs::path& p, const std::string& text) {
    std::ofstream of(p, std::ios::binary | std::ios::trunc);
    of << text;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::size_t count_of(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size()))
        ++n;
    return n;
}

static std::vector<std::string> dir_names() {  // sorted names of everything inside the scratch dir
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(scratch_dir())) names.push_back(e.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
}

// ---------------------------------------------------------------- opd runner

struct Run {
    int code = -1;
    std::string out;  // captured stdout (stderr discarded)
};

static Run run_patterns(const std::string& args) {  // args are already shell-quoted as needed
    Run r;
    const std::string cmd = "\"" + std::string(OPD_BIN) + "\" patterns " + args + " 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return r;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) r.out += buf;
    const int status = ::pclose(pipe);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

// stderr of a failing run (stdout discarded), for the exact-message checks.
static std::string run_patterns_stderr(const std::string& args) {
    std::string err;
    const std::string cmd = "\"" + std::string(OPD_BIN) + "\" patterns " + args + " 2>&1 1>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return err;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) err += buf;
    ::pclose(pipe);
    return err;
}

// ---------------------------------------------------------------- projects

// The spec's byte-exact example: pattern 0 unnamed on track 0 ("Zeta Kick") with derived length
// 1.5 + 0.25 = 1.75 and 2 notes; pattern 1 named "groove" on track 1 ("Alpha Bass"), swing 0.25, an
// EXPLICIT length_beats of 2.0 (shorter than its note's 0.0 + 3.0 would give), 1 note. Three clips:
// pattern 0 is placed by clip 1 only, pattern 1 by clips 0 and 2.
static const char* kExampleJson =
    "[\n"
    "  {\n"
    "    \"clips\": [\n"
    "      1\n"
    "    ],\n"
    "    \"index\": 0,\n"
    "    \"length_beats\": 1.75,\n"
    "    \"notes\": 2,\n"
    "    \"swing\": 0.0,\n"
    "    \"track\": 0,\n"
    "    \"track_name\": \"Zeta Kick\"\n"
    "  },\n"
    "  {\n"
    "    \"clips\": [\n"
    "      0,\n"
    "      2\n"
    "    ],\n"
    "    \"id\": \"groove\",\n"
    "    \"index\": 1,\n"
    "    \"length_beats\": 2.0,\n"
    "    \"notes\": 1,\n"
    "    \"swing\": 0.25,\n"
    "    \"track\": 1,\n"
    "    \"track_name\": \"Alpha Bass\"\n"
    "  }\n"
    "]\n";

static op::Project example_project() {
    op::Project p;
    op::Track t0;
    t0.name = "Zeta Kick";
    op::Track t1;
    t1.name = "Alpha Bass";
    p.tracks = {t0, t1};

    op::Pattern pat0;  // unnamed, track 0, derived length max(0.0 + 0.5, 1.5 + 0.25) = 1.75
    pat0.track = 0;
    pat0.notes.push_back(op::Note{0.0, 0.5, -1, 1.0});
    pat0.notes.push_back(op::Note{1.5, 0.25, -1, 1.0});

    op::Pattern pat1;  // "groove", track 1, swing 0.25, explicit 2.0 < the notes' 3.0
    pat1.track = 1;
    pat1.id = "groove";
    pat1.swing = 0.25;
    pat1.length_beats = 2.0;
    pat1.notes.push_back(op::Note{0.0, 3.0, -1, 1.0});

    p.patterns = {pat0, pat1};

    op::Clip c0;
    c0.pattern = 1;
    op::Clip c1;
    c1.pattern = 0;
    c1.start_bar = 1.0;
    op::Clip c2;
    c2.pattern = 1;
    c2.start_bar = 2.0;
    p.arrangement = {c0, c1, c2};
    return p;
}

// A legacy project: no "arrangement" key at all — every entry must report "clips": [].
static const char* kLegacyJson = R"JSON({
  "format_version": 0, "bpm": 120.0, "sample_rate": 48000, "seed": 5,
  "length_bars": 2.0, "beats_per_bar": 4,
  "tracks": [
    {"name": "LegacyOne", "voice": {"kind": "sine", "freq_hz": 220.0}},
    {"name": "LegacyTwo", "voice": {"kind": "metronome"}}
  ],
  "patterns": [
    {"track": 0, "notes": [{"t": 3.0, "dur": 0.5}, {"t": 0.0, "dur": 1.5}]},
    {"track": 1, "notes": []}
  ]
}
)JSON";

int main() {
    const fs::path dir = scratch_dir();
    if (!fs::is_directory(dir)) {
        std::printf("FATAL: cannot create temp dir %s\n", dir.string().c_str());
        return 2;
    }

    // --- empty Project → exactly "[]\n".
    CHECK(op::patterns_json(op::Project{}) == "[]\n", "patterns.empty_project_is_bracket_newline");

    // --- the spec's byte-exact example from a hand-built Project.
    const op::Project ex = example_project();
    CHECK(op::patterns_json(ex) == kExampleJson, "patterns.spec_example_byte_exact");

    // --- save/load round trip through a scratch .opj yields the same bytes.
    std::string err;
    const fs::path opj = dir / "example.opj";
    CHECK(op::save_project(opj.string(), ex, err), "patterns.save_project_succeeds");
    op::Project loaded;
    CHECK(op::load_project(opj.string(), loaded, err), "patterns.load_project_roundtrip");
    CHECK(op::patterns_json(loaded) == kExampleJson, "patterns.roundtrip_file_byte_exact");

    // --- exactly one trailing '\n'.
    const std::string out = op::patterns_json(ex);
    CHECK(out.size() >= 2 && out.back() == '\n' && out[out.size() - 2] != '\n',
          "patterns.exactly_one_trailing_newline");

    // --- model order preserved with deliberately non-alphabetical names/ids; index values 0..n-1.
    {
        op::Project p = example_project();
        p.tracks[0].name = "alpha";
        p.tracks[1].name = "Mike";
        p.tracks.push_back(op::Track{});
        p.tracks[2].name = "zulu";
        op::Pattern pat2;  // third pattern on track 1 ("Mike"), placed LAST in model order on purpose
        pat2.track = 1;
        pat2.id = "m-id";
        pat2.notes.push_back(op::Note{0.0, 1.0, -1, 1.0});
        p.patterns.push_back(pat2);
        // Model order after the re-tagging below: pattern 0 -> track 2 ("zulu", id "z-id"),
        // pattern 1 -> track 0 ("alpha", id "groove"), pattern 2 -> track 1 ("Mike", id "m-id").
        p.patterns[0].track = 2;
        p.patterns[0].id = "z-id";
        p.patterns[1].track = 0;
        const std::string o = op::patterns_json(p);
        const std::size_t pz = o.find("\"track_name\": \"zulu\"");
        const std::size_t pa = o.find("\"track_name\": \"alpha\"");
        const std::size_t pm = o.find("\"track_name\": \"Mike\"");
        CHECK(pz != std::string::npos && pa != std::string::npos && pm != std::string::npos && pz < pa && pa < pm,
              "patterns.model_order_not_sorted_by_names");
        const std::size_t iz = o.find("\"index\": 0");
        const std::size_t i1 = o.find("\"index\": 1");
        const std::size_t i2 = o.find("\"index\": 2");
        CHECK(iz != std::string::npos && i1 != std::string::npos && i2 != std::string::npos && iz < i1 && i1 < i2,
              "patterns.index_values_zero_based_in_order");
        const std::size_t dz = o.find("\"id\": \"z-id\"");
        const std::size_t dm = o.find("\"id\": \"m-id\"");
        CHECK(dz != std::string::npos && dm != std::string::npos && dz < dm,
              "patterns.ids_follow_model_order_not_alphabetical");
    }

    // --- id present when set; ABSENT (never "" / null) when not.
    CHECK(out.find("\"id\": \"groove\"") != std::string::npos, "patterns.id_present_when_set");
    CHECK(count_of(out, "\"id\":") == 1, "patterns.id_absent_when_empty_never_printed");
    CHECK(out.find("\"id\": \"\"") == std::string::npos && out.find("\"id\": null") == std::string::npos,
          "patterns.id_never_empty_string_or_null");

    // --- explicit length_beats wins both SHORTER (example: 2.0 vs the notes' 3.0) and LONGER than max(t + dur).
    CHECK(out.find("\"length_beats\": 2.0") != std::string::npos, "patterns.explicit_shorter_than_notes_wins");
    {
        op::Project p = example_project();
        p.patterns[1].length_beats = 8.0;  // notes imply 3.0 → explicit 8.0 must win
        CHECK(op::patterns_json(p).find("\"length_beats\": 8.0") != std::string::npos,
              "patterns.explicit_longer_than_notes_wins");
    }

    // --- derived length = max(t + dur) with the notes given OUT OF ORDER (kLegacyJson pattern 0:
    //     t=3.0 d=0.5 → 3.5 beats after t=0.0 d=1.5 → 1.5).
    {
        const fs::path legacy = dir / "legacy.opj";
        write_file(legacy, kLegacyJson);
        op::Project lp;
        CHECK(op::load_project(legacy.string(), lp, err) &&
                  op::patterns_json(lp).find("\"length_beats\": 3.5") != std::string::npos,
              "patterns.derived_length_out_of_order_notes");

        // --- a pattern with no notes → "length_beats": 0.0 and "notes": 0.
        const std::string lo = op::patterns_json(lp);
        CHECK(lo.find("\"length_beats\": 0.0") != std::string::npos, "patterns.no_notes_length_zero");
        CHECK(lo.find("\"notes\": 0") != std::string::npos, "patterns.no_notes_count_zero");

        // --- legacy project (no arrangement key) → EVERY entry has "clips": [].
        CHECK(count_of(lo, "\"clips\": []") == lp.patterns.size() && lp.patterns.size() == 2,
              "patterns.legacy_no_arrangement_every_entry_empty_clips");
    }

    // --- the reported length follows op::pattern_extent when the model changes between calls.
    {
        op::Project p = example_project();
        const std::string before = op::patterns_json(p);
        p.patterns[0].length_beats = 5.0;  // pattern_extent now returns the explicit value
        const std::string after = op::patterns_json(p);
        CHECK(before.find("\"length_beats\": 1.75") != std::string::npos &&
                  after.find("\"length_beats\": 5.0") != std::string::npos && before != after,
              "patterns.length_follows_pattern_extent_on_model_change");
    }

    // --- swing: default prints 0.0; set values pass through (0.25 in the example, plus 0.75).
    CHECK(out.find("\"swing\": 0.0") != std::string::npos, "patterns.swing_default_prints_zero");
    CHECK(out.find("\"swing\": 0.25") != std::string::npos, "patterns.swing_quarter_passes_through");
    {
        op::Project p = example_project();
        p.patterns[0].swing = 0.75;
        CHECK(op::patterns_json(p).find("\"swing\": 0.75") != std::string::npos,
              "patterns.swing_max_075_passes_through");
    }

    // --- note counts exact (3 and 7 notes on two patterns).
    {
        op::Project p = example_project();
        p.patterns[0].notes.clear();
        for (int i = 0; i < 3; ++i) p.patterns[0].notes.push_back(op::Note{static_cast<double>(i), 0.25, -1, 1.0});
        p.patterns[1].notes.clear();
        for (int i = 0; i < 7; ++i) p.patterns[1].notes.push_back(op::Note{static_cast<double>(i), 0.25, -1, 1.0});
        const std::string o = op::patterns_json(p);
        CHECK(o.find("\"notes\": 3") != std::string::npos && o.find("\"notes\": 7") != std::string::npos,
              "patterns.note_counts_exact");
    }

    // --- clips ascending and complete for a pattern used by non-adjacent clips; [] for an unused one.
    {
        op::Project p = example_project();
        p.patterns[0].id = "";
        op::Clip c3;  // extra placements of pattern 1 at arrangement indices 3 and 5 (index 4 unused by it)
        c3.pattern = 1;
        c3.start_bar = 3.0;
        op::Clip c4;  // a clip on pattern 0 (so index 4 belongs to pattern 0, not pattern 1)
        c4.pattern = 0;
        c4.start_bar = 4.0;
        op::Clip c5;
        c5.pattern = 1;
        c5.start_bar = 5.0;
        p.arrangement = {p.arrangement[0], p.arrangement[1], p.arrangement[2], c3, c4, c5};
        // pattern 1 sits at clips 0, 2, 3, 5 — non-adjacent, ascending; pattern 0 at 1 and 4.
        const std::string o = op::patterns_json(p);
        CHECK(o.find("\"clips\": [\n      0,\n      2,\n      3,\n      5\n    ]") != std::string::npos,
              "patterns.clips_ascending_complete_non_adjacent");
        CHECK(o.find("\"clips\": [\n      1,\n      4\n    ]") != std::string::npos,
              "patterns.clips_second_pattern_indices");
        // An unused pattern reports [].
        op::Project u = example_project();
        u.patterns.push_back(op::Pattern{});  // no clip places pattern 2
        CHECK(op::patterns_json(u).find("\"clips\": []") != std::string::npos, "patterns.clips_empty_for_unused");
    }

    // --- clip parameters (repeats, gain_db, fades, offset_beats, trim length_beats) do NOT change output.
    {
        op::Project a = example_project();
        for (op::Clip& c : a.arrangement) {
            c.repeats = 3;
            c.gain_db = -4.5;
            c.fade_in_beats = 0.25;
            c.fade_out_beats = 0.5;
            c.offset_beats = 1.0;
            c.length_beats = 1.5;
        }
        CHECK(op::patterns_json(a) == kExampleJson, "patterns.clip_parameters_do_not_change_output");
    }

    // --- track / track_name follow the pattern's own track when patterns are not in track order.
    {
        op::Project p = example_project();
        p.patterns[0].track = 1;  // first pattern plays on track 1 ("Alpha Bass")
        p.patterns[1].track = 0;  // second plays on track 0 ("Zeta Kick")
        const std::string o = op::patterns_json(p);
        CHECK(o.find("\"index\": 0,\n    \"length_beats\": 1.75") != std::string::npos &&
                  o.find("\"track\": 1") != std::string::npos &&
                  o.find("\"track_name\": \"Alpha Bass\"") < o.find("\"track_name\": \"Zeta Kick\""),
              "patterns.track_follows_pattern_not_position");
    }

    // --- track-name JSON escaping (quote + backslash).
    {
        op::Project p = example_project();
        p.tracks[0].name = std::string("a\"b\\c");
        const std::string o = op::patterns_json(p);
        CHECK(o.find("\"track_name\": \"a\\\"b\\\\c\"") != std::string::npos, "patterns.track_name_escaped_quotes_backslash");
    }

    // --- only the contract keys appear: no bpm/name/pattern/repeats/start_bar/gain_db/fades/offset/
    //     length_source/notes_per_track and no wrapper object (stdout is ONLY the array).
    {
        op::Project p = example_project();
        for (op::Clip& c : p.arrangement) {
            c.repeats = 2;
            c.gain_db = -3.0;
            c.fade_in_beats = 0.5;
            c.offset_beats = 1.0;
        }
        const std::string o = op::patterns_json(p);
        const char* forbidden[] = {"\"bpm\"", "\"length_bars\"", "\"start_bar\"", "\"repeats\"", "\"gain_db\"",
                                   "\"fade_in_beats\"", "\"fade_out_beats\"", "\"offset_beats\"", "\"pattern\":",
                                   "\"name\":", "\"length_source\"", "\"notes_per_track\""};
        bool none = true;
        for (const char* key : forbidden)
            if (o.find(key) != std::string::npos) none = false;
        CHECK(none && o[0] == '[' && count_of(o, "\"index\":") == 2, "patterns.only_contract_keys_no_wrapper");
    }

    // --- two calls on the same Project are byte-identical (pure function).
    CHECK(op::patterns_json(ex) == op::patterns_json(ex), "patterns.two_calls_byte_identical");

    // ---------------------------------------------------------------- CLI via OPD_BIN

    const std::string qopj = "\"" + opj.string() + "\"";

    // --- exit 0 and stdout == patterns_json(project).
    const Run r1 = run_patterns(qopj);
    CHECK(r1.code == 0 && r1.out == kExampleJson, "patterns.cli_exit0_stdout_equals_patterns_json");

    // --- read-only: the project file bytes are unchanged; two runs byte-identical; no files created.
    CHECK(read_file(opj) == op::project_bytes(ex), "patterns.cli_project_file_bytes_unchanged");
    const Run r2 = run_patterns(qopj);
    CHECK(r1.code == 0 && r2.code == 0 && r1.out == r2.out, "patterns.cli_two_runs_byte_identical");
    {
        write_file(dir / "sentinel.txt", "keep");
        const std::vector<std::string> before = dir_names();
        (void)run_patterns(qopj);
        const std::vector<std::string> after = dir_names();
        CHECK(before == after, "patterns.cli_creates_no_files");
    }

    // --- CLI errors: usage (exit 2, empty stdout, exact stderr line), not_found (3), parse (4).
    {
        const Run r = run_patterns("");
        const std::string e = run_patterns_stderr("");
        CHECK(r.code == 2 && r.out.empty(), "patterns.cli_no_argument_exit2_stdout_empty");
        CHECK(e.find("patterns needs exactly <project.opj>") != std::string::npos,
              "patterns.cli_no_argument_stderr_message");
    }
    {
        const Run r = run_patterns(qopj + " extra.opj");
        const std::string e = run_patterns_stderr(qopj + " extra.opj");
        CHECK(r.code == 2 && r.out.empty() && e.find("patterns needs exactly <project.opj>") != std::string::npos,
              "patterns.cli_extra_argument_exit2");
    }
    {
        const Run r = run_patterns("--json");
        const std::string e = run_patterns_stderr("--json");
        CHECK(r.code == 2 && r.out.empty() && e.find("patterns needs exactly <project.opj>") != std::string::npos,
              "patterns.cli_option_flag_exit2");
    }
    {
        const Run r = run_patterns("\"" + (dir / "does-not-exist.opj").string() + "\"");
        const std::string e = run_patterns_stderr("\"" + (dir / "does-not-exist.opj").string() + "\"");
        CHECK(r.code == 3 && r.out.empty() && e.find("not_found") != std::string::npos, "patterns.cli_missing_file_exit3");
    }
    {
        const fs::path junk = dir / "junk.opj";
        write_file(junk, "this is not json {{{");
        const Run r = run_patterns("\"" + junk.string() + "\"");
        const std::string e = run_patterns_stderr("\"" + junk.string() + "\"");
        CHECK(r.code == 4 && r.out.empty() && e.find("parse") != std::string::npos, "patterns.cli_garbage_file_exit4");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
