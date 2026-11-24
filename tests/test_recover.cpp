// Unit tests for `opd recover` (WP-0244, docs/spec/operations.md §Crash-recovery inspection and restore):
// the four report states (one exact line each, exit 0 in all of them), the restore that goes through
// op::Document::open_recovered + save_as (output bytes == the autosave's, project untouched, autosave kept),
// the document layer's own autosave cleanup when -o names the project itself, and the usage / not-found /
// parse / write exit-code contract. The command never re-decides a recovery rule and never touches anything
// but the -o file — these tests assert that from the outside. Boring assert-based runner — no framework deps,
// own main. Exit code = number of failures. Projects live in one per-process scratch dir (removed at the end);
// CLI checks run the real opd binary via OPD_BIN. The document layer itself is unit-tested in op_tests
// (WP-0157); this file never calls it, it only builds fixture files with the engine's canonical serializer.
// Run: ./build/op_recover_tests
#include <unistd.h>  // getpid

#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "engine/model.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;

static const fs::path& scratch_dir() {
    static const fs::path dir = fs::temp_directory_path() /
        ("op_recover_tests_wp0244_" + std::to_string(static_cast<long>(::getpid())));  // per-process, no collisions
    return dir;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_bytes(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    f.close();
    return !f.fail();
}

static bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Number of entries directly inside a directory (0 when it does not exist).
static std::size_t dir_entries(const fs::path& d) {
    std::error_code ec;
    std::size_t n = 0;
    for (fs::directory_iterator it(d, ec); !ec && it != fs::directory_iterator(); ++it) ++n;
    return n;
}

// ---------------------------------------------------------------- opd runner (style of test_tracks.cpp)

struct Run {
    int code = -1;
    std::string out;  // stdout
    std::string err;  // stderr (captured to a scratch file, so one run per call)
};

static Run run_opd(const std::string& args) {  // generic: args start with the subcommand ("recover ...")
    static int seq = 0;
    const fs::path errf = scratch_dir() / ("stderr_" + std::to_string(seq++) + ".txt");
    Run r;
    const std::string cmd = std::string("\"") + OPD_BIN + "\" " + args + " 2>\"" + errf.string() + "\"";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return r;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) r.out += buf;
    const int status = ::pclose(pipe);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.err = read_file(errf);
    return r;
}

// ---------------------------------------------------------------- fixtures

// 4/4 at 48 kHz with one sine track; bpm and the track name are the two values the tests compare.
static op::Project fixture_project(double bpm, const std::string& track_name) {
    op::Project p;
    p.format_version = 0;
    p.bpm = bpm;
    p.sample_rate = 48000;
    p.seed = 42;
    p.length_bars = 4.0;
    p.beats_per_bar = 4;
    op::Track t;
    t.name = track_name;
    t.voice.kind = op::VoiceKind::Sine;
    p.tracks.push_back(t);
    return p;
}

// Write a saved project and return its exact on-disk bytes.
static std::string make_project(const fs::path& path, double bpm, const std::string& name) {
    std::string err;
    if (!op::save_project(path.string(), fixture_project(bpm, name), err)) return std::string();
    return read_file(path);
}

// ------------------------------------------------------------------- main

int main() {
    std::error_code ec;
    fs::remove_all(scratch_dir(), ec);
    fs::create_directories(scratch_dir(), ec);
    std::string err;

    // === state "missing": no <proj>.autosave at all.
    const fs::path proj = scratch_dir() / "missing.opj";
    const std::string proj_bytes = make_project(proj, 120.0, "Tick");
    CHECK(!proj_bytes.empty(), "recover.fixture_saved_project_writes_bytes");

    const Run r_missing = run_opd("recover \"" + proj.string() + "\"");
    CHECK(r_missing.code == 0 && r_missing.out == "recover: " + proj.string() + ": no autosave file\n",
          "recover.report_missing_exact_line_exit_0");

    // --- default mode (no flag) is exactly --list, in every state (checked again for recoverable below).
    const Run r_missing_list = run_opd("recover \"" + proj.string() + "\" --list");
    CHECK(r_missing_list.code == 0 && r_missing_list.out == r_missing.out,
          "recover.no_flag_equals_list_missing_state");

    // --- restore in the missing state: exit 3, the document layer's own message, nothing written.
    {
        const fs::path out = scratch_dir() / "out_from_missing.opj";
        const Run r = run_opd("recover \"" + proj.string() + "\" --restore -o \"" + out.string() + "\"");
        CHECK(r.code == 3 &&
                  r.err == "error: not_found: no recoverable autosave for '" + proj.string() + "'\n" &&
                  r.out.empty(),
              "recover.restore_missing_exit_3_exact_stderr");
        CHECK(!fs::exists(out), "recover.restore_missing_writes_no_output");
    }

    // --- report writes nothing at all: the scratch subdir holds exactly the .opj before and after.
    {
        const fs::path sub = scratch_dir() / "report_noio";
        fs::create_directories(sub, ec);
        const fs::path opj = sub / "one.opj";
        make_project(opj, 120.0, "One");
        const std::size_t before = dir_entries(sub);
        const Run r = run_opd("recover \"" + opj.string() + "\"");
        CHECK(r.code == 0 && before == 1 && dir_entries(sub) == before,
              "recover.report_creates_no_files");
    }

    // === state "recoverable": <proj>.autosave holds different (newer) content.
    const fs::path pd = scratch_dir() / "differs.opj";
    const std::string saved_bytes = make_project(pd, 120.0, "Tick");
    const std::string autosave_bytes = op::project_bytes(fixture_project(137.5, "Recovered"));
    const fs::path pd_autosave = fs::path(pd.string() + ".autosave");
    CHECK(write_bytes(pd_autosave, autosave_bytes), "recover.fixture_writes_differing_autosave");
    const std::string pd_line = "recover: " + pd.string() + ": autosave differs from the saved project (recoverable)\n";

    const Run r_diff = run_opd("recover \"" + pd.string() + "\"");
    CHECK(r_diff.code == 0 && r_diff.out == pd_line, "recover.report_recoverable_exact_line_exit_0");
    const Run r_diff_list = run_opd("recover \"" + pd.string() + "\" --list");
    CHECK(r_diff_list.code == 0 && r_diff_list.out == pd_line, "recover.no_flag_equals_list_recoverable_state");

    // --- the successful restore: exact stdout, bytes == the autosave's, project kept, autosave kept.
    {
        const fs::path out = scratch_dir() / "restored.opj";
        const Run r = run_opd("recover \"" + pd.string() + "\" --restore -o \"" + out.string() + "\"");
        CHECK(r.code == 0 &&
                  r.out == "recover: " + pd.string() + ": restored " + pd.string() + ".autosave -> " +
                               out.string() + "\n" &&
                  r.err.empty(),
              "recover.restore_ok_exact_stdout_exit_0");
        CHECK(read_file(out) == autosave_bytes, "recover.restore_output_bytes_equal_autosave_bytes");
        CHECK(read_file(pd) == saved_bytes, "recover.restore_leaves_project_bytes_unchanged");
        CHECK(read_file(pd_autosave) == autosave_bytes, "recover.restore_keeps_autosave_file");

        // The output is a plain canonical .opj holding the AUTOSAVE's values, verified two ways:
        // through the engine loader and through `opd info`.
        op::Project loaded;
        CHECK(op::load_project(out.string(), loaded, err) && loaded.bpm == 137.5 &&
                  loaded.tracks.size() == 1 && loaded.tracks[0].name == "Recovered",
              "recover.restore_output_loads_with_autosave_values");
        const Run ri = run_opd("info \"" + out.string() + "\"");
        CHECK(ri.code == 0 && ri.out.find("bpm=137.50") != std::string::npos &&
                  ri.out.find("Recovered") != std::string::npos && ri.out.find("Tick") == std::string::npos,
              "recover.opd_info_reports_autosave_values_not_saved_ones");

        // --- determinism: a second restore of the same inputs is byte-identical.
        const fs::path out2 = scratch_dir() / "restored_again.opj";
        const Run r2 = run_opd("recover \"" + pd.string() + "\" --restore -o \"" + out2.string() + "\"");
        CHECK(r2.code == 0 && read_file(out2) == read_file(out), "recover.two_restores_byte_identical");
    }

    // === state "identical": <proj>.autosave byte-identical to the saved project.
    {
        const fs::path pi = scratch_dir() / "identical.opj";
        const std::string bytes = make_project(pi, 120.0, "Tick");
        const fs::path autosave = fs::path(pi.string() + ".autosave");
        CHECK(write_bytes(autosave, bytes), "recover.fixture_writes_identical_autosave");

        const Run r = run_opd("recover \"" + pi.string() + "\"");
        CHECK(r.code == 0 && r.out == "recover: " + pi.string() + ": autosave matches the saved project\n",
              "recover.report_identical_exact_line_exit_0");

        const fs::path out = scratch_dir() / "out_from_identical.opj";
        const Run rr = run_opd("recover \"" + pi.string() + "\" --restore -o \"" + out.string() + "\"");
        CHECK(rr.code == 3 &&
                  rr.err == "error: not_found: no recoverable autosave for '" + pi.string() + "'\n" &&
                  rr.out.empty(),
              "recover.restore_identical_exit_3_exact_stderr");
        CHECK(!fs::exists(out), "recover.restore_identical_writes_no_output");
        CHECK(fs::exists(autosave), "recover.restore_identical_keeps_autosave_file");
    }

    // === state "corrupt": <proj>.autosave exists but does not parse.
    {
        const fs::path pc = scratch_dir() / "corrupt.opj";
        const std::string bytes = make_project(pc, 120.0, "Tick");
        const fs::path autosave = fs::path(pc.string() + ".autosave");
        CHECK(write_bytes(autosave, "{ this is not a project at all\n"), "recover.fixture_writes_corrupt_autosave");

        const Run r = run_opd("recover \"" + pc.string() + "\"");
        CHECK(r.code == 0 && r.out == "recover: " + pc.string() + ": autosave unreadable (corrupt)\n",
              "recover.report_corrupt_exact_line_exit_0");

        const fs::path out = scratch_dir() / "out_from_corrupt.opj";
        const Run rr = run_opd("recover \"" + pc.string() + "\" --restore -o \"" + out.string() + "\"");
        CHECK(rr.code == 4 &&
                  rr.err == "error: parse: corrupt autosave '" + pc.string() + ".autosave'\n" &&
                  rr.out.empty(),
              "recover.restore_corrupt_exit_4_exact_stderr");
        CHECK(!fs::exists(out), "recover.restore_corrupt_writes_no_output");
        CHECK(read_file(pc) == bytes, "recover.corrupt_state_leaves_project_unchanged");
    }

    // === <proj>.autosave.tmp alone is not a recovery state: never read, reported or removed.
    {
        const fs::path pt = scratch_dir() / "tmp_only.opj";
        const std::string bytes = make_project(pt, 120.0, "Tick");
        const fs::path tmp = fs::path(pt.string() + ".autosave.tmp");
        CHECK(write_bytes(tmp, op::project_bytes(fixture_project(99.0, "Tmp"))), "recover.fixture_writes_tmp_file");

        const Run r = run_opd("recover \"" + pt.string() + "\"");
        CHECK(r.code == 0 && r.out == "recover: " + pt.string() + ": no autosave file\n",
              "recover.tmp_only_reports_no_autosave_file");

        const fs::path out = scratch_dir() / "out_from_tmp.opj";
        const Run rr = run_opd("recover \"" + pt.string() + "\" --restore -o \"" + out.string() + "\"");
        CHECK(rr.code == 3 && !fs::exists(out), "recover.tmp_only_restore_exit_3_writes_nothing");
        CHECK(fs::exists(tmp) && read_file(tmp) == op::project_bytes(fixture_project(99.0, "Tmp")),
              "recover.tmp_file_is_never_read_or_removed");
        CHECK(read_file(pt) == bytes, "recover.tmp_only_leaves_project_unchanged");
    }

    // === -o naming the project itself: the only case where <proj> is written; save_as clears its autosave.
    {
        const fs::path po = scratch_dir() / "overwrite.opj";
        const std::string saved = make_project(po, 120.0, "Tick");
        const std::string newer = op::project_bytes(fixture_project(137.5, "Recovered"));
        const fs::path autosave = fs::path(po.string() + ".autosave");
        write_bytes(autosave, newer);

        const Run r = run_opd("recover \"" + po.string() + "\" --restore -o \"" + po.string() + "\"");
        CHECK(r.code == 0 &&
                  r.out == "recover: " + po.string() + ": restored " + po.string() + ".autosave -> " +
                               po.string() + "\n",
              "recover.restore_into_project_itself_exit_0");
        CHECK(read_file(po) == newer, "recover.restore_into_project_itself_writes_autosave_bytes");
        CHECK(!fs::exists(autosave), "recover.restore_into_project_itself_clears_that_autosave");
    }

    // === <proj> missing (exit 3) and <proj> unparsable (exit 4, the loader's message verbatim).
    {
        const std::string nope = (scratch_dir() / "does_not_exist.opj").string();
        const Run r = run_opd("recover \"" + nope + "\"");
        CHECK(r.code == 3 && r.out.empty() && r.err == "error: not_found: " + nope + "\n",
              "recover.missing_project_report_exit_3");
        const fs::path out = scratch_dir() / "out_from_nope.opj";
        const Run rr = run_opd("recover \"" + nope + "\" --restore -o \"" + out.string() + "\"");
        CHECK(rr.code == 3 && !fs::exists(out), "recover.missing_project_restore_exit_3_writes_nothing");
    }
    {
        const fs::path bad = scratch_dir() / "garbage.opj";
        CHECK(write_bytes(bad, "{ this is not json at all\n"), "recover.fixture_writes_garbage_project");
        op::Project junk;
        std::string loader_err;
        const bool load_failed = !op::load_project(bad.string(), junk, loader_err);
        const Run r = run_opd("recover \"" + bad.string() + "\"");
        CHECK(load_failed && r.code == 4 && r.out.empty() &&
                  r.err == "error: parse: " + loader_err + "\n",
              "recover.unparsable_project_report_exit_4_loader_message");
        const fs::path out = scratch_dir() / "out_from_garbage.opj";
        const Run rr = run_opd("recover \"" + bad.string() + "\" --restore -o \"" + out.string() + "\"");
        CHECK(rr.code == 4 && rr.out.empty() && rr.err == "error: parse: " + loader_err + "\n" &&
                  !fs::exists(out),
              "recover.unparsable_project_restore_exit_4_writes_nothing");
    }

    // === unwritable -o (its directory does not exist): exit 5, project and autosave unchanged.
    {
        const fs::path out = scratch_dir() / "no_such_dir" / "out.opj";
        const Run r = run_opd("recover \"" + pd.string() + "\" --restore -o \"" + out.string() + "\"");
        CHECK(r.code == 5 && r.out.empty() && !fs::exists(out), "recover.unwritable_output_exit_5");
        CHECK(read_file(pd) == saved_bytes && read_file(pd_autosave) == autosave_bytes,
              "recover.write_failure_leaves_project_and_autosave_unchanged");
    }

    // === usage errors: exit 2, the exact message last on stderr, nothing written.
    const fs::path usage_out = scratch_dir() / "never_written.opj";
    {
        const Run r = run_opd("recover");
        CHECK(r.code == 2 && r.out.empty() &&
                  ends_with(r.err, "error: usage: recover needs <proj.opj> [--list | --restore -o <out.opj>]\n"),
              "recover.usage_no_project_exit_2");
    }
    {
        const Run r = run_opd("recover \"" + pd.string() + "\" --frobnicate");
        CHECK(r.code == 2 && r.out.empty() && ends_with(r.err, "error: usage: unknown arg '--frobnicate'\n"),
              "recover.usage_unknown_arg_exit_2");
    }
    {
        const Run r = run_opd("recover \"" + pd.string() + "\" --list --restore -o \"" + usage_out.string() + "\"");
        CHECK(r.code == 2 && r.out.empty() &&
                  ends_with(r.err, "error: usage: --list and --restore are mutually exclusive\n"),
              "recover.usage_list_and_restore_exit_2");
    }
    {
        const Run r = run_opd("recover \"" + pd.string() + "\" --restore");
        CHECK(r.code == 2 && r.out.empty() &&
                  ends_with(r.err, "error: usage: recover --restore needs -o <out.opj>\n"),
              "recover.usage_restore_without_o_exit_2");
    }
    {
        const Run r = run_opd("recover \"" + pd.string() + "\" -o \"" + usage_out.string() + "\"");
        CHECK(r.code == 2 && r.out.empty() && ends_with(r.err, "error: usage: -o needs --restore\n"),
              "recover.usage_o_without_restore_exit_2");
    }
    CHECK(!fs::exists(usage_out), "recover.usage_errors_write_no_output_file");

    // === determinism of the report: two runs on the same inputs are byte-identical.
    {
        const Run a = run_opd("recover \"" + pd.string() + "\" --list");
        const Run b = run_opd("recover \"" + pd.string() + "\" --list");
        CHECK(a.code == 0 && b.code == 0 && a.out == b.out, "recover.report_two_runs_byte_identical");
    }

    fs::remove_all(scratch_dir(), ec);
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
