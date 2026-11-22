// Unit tests for WP-0210 (`opd timeline`): the three public renderer helpers op::beats_at_bar,
// op::sample_at_beat and op::bpm_at_beat, plus the byte-exact JSON of `opd timeline <proj> [--bars N]`.
// Boring assert-based runner — no framework deps, own main. Exit code = number of failures. All project
// files are written into one per-process scratch directory under temp_directory_path() (pid in the name)
// and removed at the end; nothing outside it is created. Runs the opd binary via OPD_BIN.
// Run: ./build/op_timeline_tests
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/model.hpp"
#include "engine/render.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;

// ---------------------------------------------------------------- scratch directory + file helpers

static fs::path scratch_dir() {
    static const fs::path dir = [] {
        const fs::path p = fs::temp_directory_path() /
                           ("op_timeline_tests_" + std::to_string(static_cast<long>(::getpid())));
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

// ---------------------------------------------------------------- opd runner

struct Run {
    int code = -1;
    std::string out;   // captured stdout (stderr discarded)
};

static Run run_timeline(const std::string& args) {  // args are already shell-quoted as needed
    Run r;
    const std::string cmd = "\"" + std::string(OPD_BIN) + "\" timeline " + args + " 2>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return r;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) r.out += buf;
    const int status = ::pclose(pipe);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

// stderr of a failing run (stdout discarded), for the exact-message checks.
static std::string run_timeline_stderr(const std::string& args) {
    std::string err;
    const std::string cmd = "\"" + std::string(OPD_BIN) + "\" timeline " + args + " 2>&1 1>/dev/null";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return err;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) err += buf;
    ::pclose(pipe);
    return err;
}

static int count_rows(const std::string& json) {
    int n = 0;
    for (std::size_t pos = json.find("\"bar\":"); pos != std::string::npos; pos = json.find("\"bar\":", pos + 6))
        ++n;
    return n;
}

// ---------------------------------------------------------------- project texts

// The tempo-map project of tests/fixtures/goldens/tempo.opj (120 bpm, 72 bpm from bar 4, 4/4, 48 kHz).
static const char* kTempo = R"JSON({
  "format_version": 0, "bpm": 120.0, "sample_rate": 48000, "seed": 777,
  "length_bars": 8, "beats_per_bar": 4,
  "tempo_map": [ { "bar": 0.0, "bpm": 120.0 }, { "bar": 4.0, "bpm": 72.0 } ],
  "tracks": [ { "name": "Lead", "voice": { "kind": "sine", "freq_hz": 440.0 } } ],
  "patterns": [ { "track": 0, "notes": [ { "t": 0.0, "dur": 1.0 } ] } ]
})JSON";

// Constant-tempo project (no tempo_map): 96 bpm → exactly 30000 samples per beat at 48 kHz, 4 bars.
static const char* kPlain = R"JSON({
  "format_version": 0, "bpm": 96.0, "sample_rate": 48000, "seed": 1,
  "length_bars": 4, "beats_per_bar": 4,
  "tracks": [ { "name": "Lead", "voice": { "kind": "sine", "freq_hz": 440.0 } } ],
  "patterns": [ { "track": 0, "notes": [ { "t": 0.0, "dur": 1.0 } ] } ]
})JSON";

// Three-quarter bar (3/4 normalises to beats_per_bar 3, no stored signature) and 6/8 (stored).
static const char* kSig34 = R"JSON({
  "format_version": 0, "bpm": 120.0, "sample_rate": 48000, "seed": 34,
  "length_bars": 3, "time_signature": { "num": 3, "den": 4 },
  "tracks": [ { "name": "Lead", "voice": { "kind": "sine", "freq_hz": 440.0 } } ],
  "patterns": [ { "track": 0, "notes": [ { "t": 0.0, "dur": 1.0 } ] } ]
})JSON";

static const char* kSig68 = R"JSON({
  "format_version": 0, "bpm": 120.0, "sample_rate": 48000, "seed": 68,
  "length_bars": 2, "time_signature": { "num": 6, "den": 8 },
  "tracks": [ { "name": "Lead", "voice": { "kind": "sine", "freq_hz": 440.0 } } ],
  "patterns": [ { "track": 0, "notes": [ { "t": 0.0, "dur": 1.0 } ] } ]
})JSON";

// Fractional length: 2.5 bars → rows for bars 0, 1 and the partial bar 2 only.
static const char* kPartial = R"JSON({
  "format_version": 0, "bpm": 120.0, "sample_rate": 48000, "seed": 25,
  "length_bars": 2.5, "beats_per_bar": 4,
  "tracks": [ { "name": "Lead", "voice": { "kind": "sine", "freq_hz": 440.0 } } ],
  "patterns": [ { "track": 0, "notes": [ { "t": 0.0, "dur": 1.0 } ] } ]
})JSON";

// The first five rows of the tempo project exactly as the task's contract example specifies.
static const char* kTempoHead5 =
    "{\n"
    "  \"sample_rate\": 48000,\n"
    "  \"beats_per_bar\": 4,\n"
    "  \"time_signature\": \"4/4\",\n"
    "  \"length_bars\": 8.000000,\n"
    "  \"bars\": [\n"
    "    {\"bar\": 0, \"start_beat\": 0.000000, \"start_sample\": 0, \"start_seconds\": 0.000000, \"bpm\": 120.00},\n"
    "    {\"bar\": 1, \"start_beat\": 4.000000, \"start_sample\": 96000, \"start_seconds\": 2.000000, \"bpm\": 120.00},\n"
    "    {\"bar\": 2, \"start_beat\": 8.000000, \"start_sample\": 192000, \"start_seconds\": 4.000000, \"bpm\": 120.00},\n"
    "    {\"bar\": 3, \"start_beat\": 12.000000, \"start_sample\": 288000, \"start_seconds\": 6.000000, \"bpm\": 120.00},\n"
    "    {\"bar\": 4, \"start_beat\": 16.000000, \"start_sample\": 384000, \"start_seconds\": 8.000000, \"bpm\": 72.00}\n"
    "  ]\n"
    "}\n";

// Same header, three rows (the --bars 3 contract: no comma after the last row).
static const char* kTempoHead3 =
    "{\n"
    "  \"sample_rate\": 48000,\n"
    "  \"beats_per_bar\": 4,\n"
    "  \"time_signature\": \"4/4\",\n"
    "  \"length_bars\": 8.000000,\n"
    "  \"bars\": [\n"
    "    {\"bar\": 0, \"start_beat\": 0.000000, \"start_sample\": 0, \"start_seconds\": 0.000000, \"bpm\": 120.00},\n"
    "    {\"bar\": 1, \"start_beat\": 4.000000, \"start_sample\": 96000, \"start_seconds\": 2.000000, \"bpm\": 120.00},\n"
    "    {\"bar\": 2, \"start_beat\": 8.000000, \"start_sample\": 192000, \"start_seconds\": 4.000000, \"bpm\": 120.00}\n"
    "  ]\n"
    "}\n";

// ------------------------------------------------------------------- the tests

int main() {
    const fs::path dir = scratch_dir();
    const std::string tempo_opj = (dir / "tempo.opj").string();
    const std::string plain_opj = (dir / "plain.opj").string();
    const std::string sig34_opj = (dir / "sig_3_4.opj").string();
    const std::string sig68_opj = (dir / "sig_6_8.opj").string();
    const std::string partial_opj = (dir / "partial.opj").string();
    write_file(tempo_opj, kTempo);
    write_file(plain_opj, kPlain);
    write_file(sig34_opj, kSig34);
    write_file(sig68_opj, kSig68);
    write_file(partial_opj, kPartial);

    std::string err;
    op::Project tempo;
    CHECK(op::load_project(tempo_opj, tempo, err), "engine: tempo-map project loads");

    // ---- beats_at_bar: bar numbering is bar * beats_per_bar, 0-based, fractional ok. ----
    CHECK(op::beats_at_bar(tempo, 0.0) == 0.0 && op::beats_at_bar(tempo, 4.0) == 16.0,
          "engine: beats_at_bar on the tempo project (bar 4 = beat 16)");
    CHECK(op::beats_at_bar(tempo, 7.5) == 30.0, "engine: beats_at_bar accepts a fractional bar");

    // ---- sample_at_beat across the segment boundary at bar 4 (beat 16). ----
    CHECK(op::sample_at_beat(tempo, 12.0) == 288000 && op::sample_at_beat(tempo, 15.75) == 378000,
          "engine: sample_at_beat inside the first segment (120 bpm)");
    CHECK(op::sample_at_beat(tempo, 16.0) == 384000 && op::sample_at_beat(tempo, 20.0) == 544000,
          "engine: sample_at_beat from the segment boundary at bar 4 (72 bpm)");

    // ---- bpm_at_beat: segment selection at the boundary uses the last point at or before the beat. ----
    CHECK(op::bpm_at_beat(tempo, 0.0) == 120.0 && op::bpm_at_beat(tempo, 15.75) == 120.0,
          "engine: bpm_at_beat is 120 before the boundary (beat 15.75)");
    CHECK(op::bpm_at_beat(tempo, 16.0) == 72.0 && op::bpm_at_beat(tempo, 40.0) == 72.0,
          "engine: bpm_at_beat is 72 from the boundary (beat 16.0)");

    // ---- Past the end: sample_at_beat is NOT clamped, sample_at_bar still is. ----
    CHECK(op::total_samples_for(tempo) == 1024000, "engine: total_samples_for of the tempo project is 1024000");
    CHECK(op::sample_at_beat(tempo, 36.0) == 1184000,
          "engine: sample_at_beat past the end is unclamped (1184000 > total)");
    CHECK(op::sample_at_bar(tempo, 99.0) == 1024000 && op::sample_at_bar(tempo, 8.0) == 1024000,
          "engine: sample_at_bar still clamps to total_samples_for");

    // ---- Constant-tempo project: legacy path, project bpm at any beat. ----
    op::Project plain;
    CHECK(op::load_project(plain_opj, plain, err) && plain.tempo_map.empty(),
          "engine: constant-tempo project loads with an empty tempo_map");
    CHECK(op::beats_at_bar(plain, 4.0) == 16.0 && op::sample_at_beat(plain, 16.0) == 480000,
          "engine: beats_at_bar + sample_at_beat on the constant-tempo project");
    CHECK(op::bpm_at_beat(plain, 0.0) == 96.0 && op::bpm_at_beat(plain, 12345.75) == 96.0,
          "engine: bpm_at_beat returns the project bpm at any beat without a tempo map");
    CHECK(op::sample_at_beat(plain, 12345.75) == 370372500LL && op::sample_at_bar(plain, 99.0) == 480000,
          "engine: constant-tempo sample_at_beat unclamped, sample_at_bar clamped");

    // ---- Three-beat bar: beats_per_bar follows the signature for all three functions. ----
    op::Project sig34;
    CHECK(op::load_project(sig34_opj, sig34, err) && sig34.beats_per_bar == 3,
          "engine: 3/4 project loads with beats_per_bar 3");
    CHECK(op::beats_at_bar(sig34, 1.0) == 3.0 && op::sample_at_beat(sig34, 3.0) == 72000 &&
              op::bpm_at_beat(sig34, 3.0) == 120.0,
          "engine: all three helpers on the 3-beat project");

    // ---- opd timeline: byte-exact JSON for the tempo project (--bars 5 = the contract example). ----
    const Run five = run_timeline("\"" + tempo_opj + "\" --bars 5");
    CHECK(five.code == 0 && five.out == kTempoHead5, "cli: --bars 5 output is byte-identical to the contract example");

    // ---- Default: one row per bar; last three rows of the 8-bar tempo project. ----
    const Run all = run_timeline("\"" + tempo_opj + "\"");
    CHECK(all.code == 0 && count_rows(all.out) == 8, "cli: default prints one row per bar (8 rows)");
    CHECK(all.out.find("    {\"bar\": 5, \"start_beat\": 20.000000, \"start_sample\": 544000,"
                       " \"start_seconds\": 11.333333, \"bpm\": 72.00},\n") != std::string::npos &&
              all.out.find("    {\"bar\": 6, \"start_beat\": 24.000000, \"start_sample\": 704000,"
                           " \"start_seconds\": 14.666667, \"bpm\": 72.00},\n") != std::string::npos &&
              all.out.find("    {\"bar\": 7, \"start_beat\": 28.000000, \"start_sample\": 864000,"
                           " \"start_seconds\": 18.000000, \"bpm\": 72.00}\n") != std::string::npos,
          "cli: rows 5-7 of the tempo project (72 bpm) are exact");
    const Run twice = run_timeline("\"" + tempo_opj + "\"");
    CHECK(twice.code == 0 && twice.out == all.out, "cli: two runs are byte-identical (determinism)");

    // ---- --bars N prints exactly the first N rows (no comma after the last one). ----
    const Run three = run_timeline("\"" + tempo_opj + "\" --bars 3");
    CHECK(three.code == 0 && three.out == kTempoHead3, "cli: --bars 3 prints exactly the first three rows");

    // ---- A final partial bar gets its own row and nothing comes after it. ----
    const Run part = run_timeline("\"" + partial_opj + "\"");
    CHECK(part.code == 0 && count_rows(part.out) == 3 &&
              part.out.find("    {\"bar\": 2, \"start_beat\": 8.000000, \"start_sample\": 192000,"
                            " \"start_seconds\": 4.000000, \"bpm\": 120.00}\n") != std::string::npos &&
              part.out.find("\"bar\": 3") == std::string::npos,
          "cli: length_bars 2.5 gets a row for the partial bar 2 and no further rows");

    // ---- Time signatures in the header + rows (3/4 normalises to 3 beats; 6/8 keeps its label). ----
    const Run s34 = run_timeline("\"" + sig34_opj + "\"");
    CHECK(s34.code == 0 &&
              s34.out.find("  \"beats_per_bar\": 3,\n  \"time_signature\": \"3/4\",\n") != std::string::npos,
          "cli: 3/4 header shows beats_per_bar 3 and time_signature 3/4");
    CHECK(s34.out.find("    {\"bar\": 1, \"start_beat\": 3.000000, \"start_sample\": 72000,"
                       " \"start_seconds\": 1.500000, \"bpm\": 120.00},\n") != std::string::npos &&
              count_rows(s34.out) == 3,
          "cli: 3/4 bar 1 row is exact and every bar gets a row");
    const Run s68 = run_timeline("\"" + sig68_opj + "\"");
    CHECK(s68.code == 0 &&
              s68.out.find("  \"beats_per_bar\": 3,\n  \"time_signature\": \"6/8\",\n") != std::string::npos,
          "cli: 6/8 header shows beats_per_bar 3 and time_signature 6/8");

    // ---- Exit codes: 2 usage (stdout empty), 3 not-found, 4 parse. ----
    const Run no_args = run_timeline("");
    CHECK(no_args.code == 2 && no_args.out.empty(), "cli: no project argument → exit 2 with empty stdout");
    const Run unknown_opt = run_timeline("\"" + tempo_opj + "\" --json");
    CHECK(unknown_opt.code == 2 && unknown_opt.out.empty(), "cli: unknown option → exit 2 with empty stdout");
    const Run valueless = run_timeline("\"" + tempo_opj + "\" --bars");
    CHECK(valueless.code == 2 && valueless.out.empty(), "cli: valueless --bars → exit 2 with empty stdout");
    const Run second_pos = run_timeline("\"" + tempo_opj + "\" \"" + plain_opj + "\"");
    CHECK(second_pos.code == 2 && second_pos.out.empty(), "cli: second positional → exit 2 with empty stdout");
    const Run bad_bars = run_timeline("\"" + tempo_opj + "\" --bars 1.5");
    CHECK(bad_bars.code == 2 && bad_bars.out.empty(), "cli: non-integer --bars → exit 2 with empty stdout");
    const Run zero_bars = run_timeline("\"" + tempo_opj + "\" --bars 0");
    CHECK(zero_bars.code == 2 && zero_bars.out.empty(), "cli: --bars 0 → exit 2 with empty stdout");
    const Run too_many = run_timeline("\"" + tempo_opj + "\" --bars 9");
    const std::string too_many_err = run_timeline_stderr("\"" + tempo_opj + "\" --bars 9");
    const std::string want_err = "error: usage: --bars 9 is larger than the project's 8 bars\n";
    CHECK(too_many.code == 2 && too_many.out.empty() &&
              too_many_err.size() >= want_err.size() &&
              too_many_err.compare(too_many_err.size() - want_err.size(), want_err.size(), want_err) == 0,
          "cli: --bars above the project's bar count → exit 2 with the exact message");
    const Run missing = run_timeline("\"" + (dir / "nope.opj").string() + "\"");
    CHECK(missing.code == 3 && missing.out.empty(), "cli: missing file → exit 3 with empty stdout");
    const fs::path bad_opj = dir / "bad.opj";
    write_file(bad_opj, "this is not json {{{");
    const Run unparsable = run_timeline("\"" + bad_opj.string() + "\"");
    CHECK(unparsable.code == 4 && unparsable.out.empty(), "cli: unparsable project → exit 4 with empty stdout");

    // ---- Read-only: the project bytes are unchanged and no new files appear next to it. ----
    const std::string before = read_file(tempo_opj);
    std::vector<std::string> names_before;
    for (const auto& e : fs::directory_iterator(dir)) names_before.push_back(e.path().filename().string());
    const Run again = run_timeline("\"" + tempo_opj + "\" --bars 8");
    std::vector<std::string> names_after;
    for (const auto& e : fs::directory_iterator(dir)) names_after.push_back(e.path().filename().string());
    CHECK(again.code == 0 && read_file(tempo_opj) == before, "cli: the project file is byte-identical after a run");
    CHECK(names_before == names_after, "cli: the run creates no files");

    fs::remove_all(dir);
    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURES\n", g_failures);
    return g_failures;
}
