// Unit tests for `opd new --time-signature N/D` (WP-0246, docs/spec/project-model.md
// §New project time signature — additive): the flag is DEFINED as the set_time_signature operation
// applied to the project the same `opd new` invocation would otherwise write — byte-identical to
// `opd new` + the journal {set_bpm, set_time_signature, set_length_bars}. Boring assert-based runner
// — no framework deps, own main. Exit code = number of failures. Projects live in one per-process
// scratch dir (removed at the end); CLI checks run the real opd binary via OPD_BIN.
// Run: ./build/op_new_opts_tests
#include <unistd.h>  // getpid

#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/model.hpp"
#include "engine/ops.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;

static const fs::path& scratch_dir() {
    static const fs::path dir = fs::temp_directory_path() /
        ("op_new_opts_tests_wp0246_" + std::to_string(static_cast<long>(::getpid())));  // per-process, no collisions
    return dir;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void write_text(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// ---------------------------------------------------------------- opd runner (style of test_tracks.cpp)

struct Run {
    int code = -1;
    std::string out;  // stdout
    std::string err;  // stderr (captured to a scratch file, so one run per call)
};

static Run run_opd(const std::string& args) {  // generic: args start with the subcommand ("new ...")
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

// ---------------------------------------------------------------- helpers

// The reference for the byte-equality contract: op::project_bytes of new_project's default project
// with the three ops applied through op::apply_op, in the task table's order.
static std::string applied_bytes(double bpm, int num, int den, int bars) {
    op::Project p = op::new_project(op::NewProjectSpec{});
    std::string err;
    const bool ok =
        op::apply_op(p, nlohmann::json{{"op", "set_bpm"}, {"bpm", bpm}}, err) &&
        op::apply_op(p, nlohmann::json{{"op", "set_time_signature"}, {"num", num}, {"den", den}}, err) &&
        op::apply_op(p, nlohmann::json{{"op", "set_length_bars"}, {"length_bars", bars}}, err);
    return ok ? op::project_bytes(p) : std::string("<apply_op failed: ") + err + ">";
}

// One byte-equality row of the task table: bpm text as passed to --bpm AND the journal line.
struct Row {
    const char* bpm_text;
    const char* sig;
    int num, den;
    int bars;
};
static const Row kRows[] = {
    {"90.0",   "6/8",   6,  8,  7},
    {"140.5",  "3/4",   3,  4,  1},
    {"120.0",  "2/2",   2,  2,  16},
    {"72.0",   "12/16", 12, 16, 4096},
    {"0.5",    "32/4",  32, 4,  2},
    {"999.99", "4/4",   4,  4,  8},
    {"121.0",  "12/8",  12, 8,  33},
    {"119",    "1/2",   1,  2,  4},
};

// error row of the task table: args + the complete error line + a fresh out path per run
struct ErrRow {
    const char* name;
    const char* args;           // everything after `opd new "<out>"`
    const char* line;           // the complete error line, expected verbatim in stderr
};
static const ErrRow kErrRows[] = {
    {"err_abc",          "--time-signature abc",
     "error: usage: bad --time-signature value 'abc': expected N/D (e.g. 6/8)"},
    {"err_6x8",          "--time-signature 6x8",
     "error: usage: bad --time-signature value '6x8': expected N/D (e.g. 6/8)"},
    {"err_6_8_4",        "--time-signature 6/8/4",
     "error: usage: bad --time-signature value '6/8/4': den must be 2, 4, 8 or 16"},
    {"err_slash_8",      "--time-signature /8",
     "error: usage: bad --time-signature value '/8': num must be an integer in [1, 32]"},
    {"err_6_slash",      "--time-signature 6/",
     "error: usage: bad --time-signature value '6/': den must be 2, 4, 8 or 16"},
    {"err_7_8",          "--time-signature 7/8",
     "error: usage: bad --time-signature value '7/8': bar must be a whole number of quarter-note beats"},
    {"err_4_3",          "--time-signature 4/3",
     "error: usage: bad --time-signature value '4/3': den must be 2, 4, 8 or 16"},
    {"err_0_4",          "--time-signature 0/4",
     "error: usage: bad --time-signature value '0/4': num must be an integer in [1, 32]"},
    {"err_33_4",         "--time-signature 33/4",
     "error: usage: bad --time-signature value '33/4': num must be an integer in [1, 32]"},
    {"err_32_2",         "--time-signature 32/2",
     "error: usage: bad --time-signature value '32/2': bar must be at most 32 quarter-note beats"},
    {"err_no_value",     "--time-signature",
     "error: usage: missing value for --time-signature"},
    {"err_conflict_a",   "--time-signature 3/4 --beats-per-bar 5",
     "error: usage: --time-signature '3/4' conflicts with --beats-per-bar 5 (beats_per_bar would be 3)"},
    {"err_conflict_b",   "--beats-per-bar 5 --time-signature 3/4",
     "error: usage: --time-signature '3/4' conflicts with --beats-per-bar 5 (beats_per_bar would be 3)"},
};

// File-name-safe form of a signature: "6/8" → "6_8" (scratch files never contain '/').
static std::string sig_name(const char* s) {
    std::string out(s);
    for (char& c : out)
        if (c == '/') c = '_';
    return out;
}

int main() {
    std::error_code ec;
    fs::remove_all(scratch_dir(), ec);
    fs::create_directories(scratch_dir(), ec);
    if (ec) {
        std::printf("FATAL: cannot create temp dir %s\n", scratch_dir().string().c_str());
        return 2;
    }

    // ================= the 8 byte-equality rows =================
    // CLI bytes == op::project_bytes(the three ops applied by op::apply_op) AND CLI bytes ==
    // `opd apply` of exactly the three journal lines from the task table.
    std::string row0_bytes;  // 90.0 6/8 7 — reused by the flag-order checks
    for (std::size_t i = 0; i < sizeof kRows / sizeof kRows[0]; ++i) {
        const Row& r = kRows[i];
        const std::string tag = std::to_string(i);
        const fs::path a = scratch_dir() / ("a" + tag + ".opj");
        const Run ra = run_opd("new \"" + a.string() + "\" --bpm " + r.bpm_text +
                               " --time-signature " + r.sig + " --bars " + std::to_string(r.bars));
        const std::string want = applied_bytes(std::stod(r.bpm_text), r.num, r.den, r.bars);
        CHECK(ra.code == 0 && read_file(a) == want, ("row" + tag + "_cli_equals_apply_op_bytes").c_str());
        if (i == 0) row0_bytes = read_file(a);

        const fs::path b = scratch_dir() / ("b" + tag + ".opj");
        const fs::path jl = scratch_dir() / ("j" + tag + ".opjl");
        write_text(jl, std::string("{\"op\":\"set_bpm\",\"bpm\":") + r.bpm_text + "}\n" +
                       "{\"op\":\"set_time_signature\",\"num\":" + std::to_string(r.num) +
                       ",\"den\":" + std::to_string(r.den) + "}\n" +
                       "{\"op\":\"set_length_bars\",\"length_bars\":" + std::to_string(r.bars) + "}\n");
        const Run rb = run_opd("new \"" + b.string() + "\"");
        const Run rc = run_opd("apply \"" + b.string() + "\" \"" + jl.string() +
                               "\" -o \"" + (scratch_dir() / ("r" + tag + ".opj")).string() + "\"");
        CHECK(rb.code == 0 && rc.code == 0 &&
                  read_file(a) == read_file(scratch_dir() / ("r" + tag + ".opj")),
              ("row" + tag + "_cli_equals_opd_apply_journal_bytes").c_str());
    }

    // ================= defaults unchanged =================
    const fs::path def = scratch_dir() / "default.opj";
    const Run rdef = run_opd("new \"" + def.string() + "\"");
    CHECK(rdef.code == 0 && read_file(def) == op::project_bytes(op::new_project(op::NewProjectSpec{})),
          "defaults_no_flag_bytes_unchanged");
    {
        const nlohmann::json j = nlohmann::json::parse(read_file(def));
        CHECK(!j.contains("time_signature") && j.at("beats_per_bar").get<int>() == 4 &&
                  j.at("bpm").get<double>() == 120.0 && j.at("length_bars").get<double>() == 4.0,
              "defaults_no_ts_key_bpb4_bpm120_bars4");
    }
    {
        const fs::path p44 = scratch_dir() / "d44.opj";
        const Run r44 = run_opd("new \"" + p44.string() + "\" --time-signature 4/4");
        CHECK(r44.code == 0 && read_file(p44) == read_file(def),
              "four_four_is_byte_identical_to_no_flag");
    }

    // ================= normalization: den == 4 stores NO time_signature key =================
    {
        const fs::path p = scratch_dir() / "ts_3_4.opj";
        const Run r = run_opd("new \"" + p.string() + "\" --time-signature 3/4");
        const std::string bytes = read_file(p);
        const nlohmann::json j = nlohmann::json::parse(bytes);
        CHECK(r.code == 0 && bytes.find("time_signature") == std::string::npos &&
                  j.at("beats_per_bar").get<int>() == 3,
              "three_four_stores_no_key_derives_3");
    }
    {
        const fs::path p = scratch_dir() / "ts_32_4.opj";
        const Run r = run_opd("new \"" + p.string() + "\" --time-signature 32/4");
        const std::string bytes = read_file(p);
        const nlohmann::json j = nlohmann::json::parse(bytes);
        CHECK(r.code == 0 && bytes.find("time_signature") == std::string::npos &&
                  j.at("beats_per_bar").get<int>() == 32,
              "thirty_two_four_stores_no_key_derives_32");
    }

    // ================= den != 4 stores num/den (and derives beats_per_bar) =================
    {
        const fs::path p = scratch_dir() / "ts_6_8.opj";
        const Run r = run_opd("new \"" + p.string() + "\" --time-signature 6/8");
        const nlohmann::json j = nlohmann::json::parse(read_file(p));
        CHECK(r.code == 0 && j.at("time_signature").at("num").get<int>() == 6 &&
                  j.at("time_signature").at("den").get<int>() == 8 &&
                  j.at("beats_per_bar").get<int>() == 3,
              "six_eight_stores_6_over_8_derives_3");
    }
    {
        const fs::path p = scratch_dir() / "ts_2_2.opj";
        const Run r = run_opd("new \"" + p.string() + "\" --time-signature 2/2");
        const std::string bytes = read_file(p);
        const nlohmann::json j = nlohmann::json::parse(bytes);
        CHECK(r.code == 0 && j.at("time_signature").at("num").get<int>() == 2 &&
                  j.at("time_signature").at("den").get<int>() == 2 &&
                  j.at("beats_per_bar").get<int>() == 4,
              "two_two_stores_2_over_2_even_though_it_derives_4");
        const fs::path b4 = scratch_dir() / "bpb4.opj";
        const Run r4 = run_opd("new \"" + b4.string() + "\" --beats-per-bar 4");
        CHECK(r4.code == 0 && bytes != read_file(b4),
              "two_two_bytes_differ_from_bare_beats_per_bar_4");
    }
    {
        const fs::path p = scratch_dir() / "ts_12_16.opj";
        const Run r = run_opd("new \"" + p.string() + "\" --time-signature 12/16");
        const nlohmann::json j = nlohmann::json::parse(read_file(p));
        CHECK(r.code == 0 && j.at("time_signature").at("num").get<int>() == 12 &&
                  j.at("time_signature").at("den").get<int>() == 16 &&
                  j.at("beats_per_bar").get<int>() == 3,
              "twelve_sixteen_stores_12_over_16_derives_3");
    }

    // ================= canonical file + round trip =================
    {
        op::Project loaded;
        std::string err;
        CHECK(op::load_project((scratch_dir() / "ts_6_8.opj").string(), loaded, err) &&
                  read_file(scratch_dir() / "ts_6_8.opj") == op::project_bytes(loaded),
              "written_file_is_canonical_equals_project_bytes_of_load");
    }
    {
        const fs::path jl = scratch_dir() / "empty.opjl";
        write_text(jl, "");
        const fs::path src = scratch_dir() / "ts_2_2.opj";
        const fs::path out = scratch_dir() / "ts_2_2_rt.opj";
        const Run r = run_opd("apply \"" + src.string() + "\" \"" + jl.string() +
                              "\" -o \"" + out.string() + "\"");
        CHECK(r.code == 0 && read_file(out) == read_file(src),
              "round_trip_through_apply_of_empty_journal_bytes_identical");
    }

    // ================= leading zeros =================
    {
        const fs::path lz = scratch_dir() / "lz.opj";
        const fs::path plain = scratch_dir() / "plain48.opj";
        const Run r1 = run_opd("new \"" + lz.string() + "\" --time-signature 04/08");
        const Run r2 = run_opd("new \"" + plain.string() + "\" --time-signature 4/8");
        CHECK(r1.code == 0 && r2.code == 0 && read_file(lz) == read_file(plain),
              "leading_zeros_04_08_equals_4_8");
    }

    // ================= accepted boundaries =================
    {
        bool ok = true;
        for (const char* s : {"1/2", "1/4", "4/16"}) {  // N = 1 (whole-quarter bars only)
            const fs::path p = scratch_dir() / ("bnd_" + sig_name(s) + ".opj");
            ok = ok && run_opd("new \"" + p.string() + "\" --time-signature " + s).code == 0;
        }
        CHECK(ok, "accepted_boundaries_num_1");
    }
    {
        bool ok = true;
        for (const char* s : {"32/4", "32/16", "16/2"}) {  // N = 32 / 32-beat bars, den 2
            const fs::path p = scratch_dir() / ("bnd_" + sig_name(s) + ".opj");
            ok = ok && run_opd("new \"" + p.string() + "\" --time-signature " + s).code == 0;
        }
        CHECK(ok, "accepted_boundaries_num_32_and_den_2");
    }
    {
        bool ok = true;
        for (const char* s : {"12/16", "4/8", "2/2", "3/4", "6/8", "4/4"}) {
            const fs::path p = scratch_dir() / ("bnd2_" + sig_name(s) + ".opj");
            ok = ok && run_opd("new \"" + p.string() + "\" --time-signature " + s).code == 0;
        }
        CHECK(ok, "accepted_meters_all_exit_0");
    }

    // ================= every error row: exit 2, exact line, nothing written =================
    for (const ErrRow& e : kErrRows) {
        const fs::path out = scratch_dir() / (std::string("errout_") + e.name + ".opj");
        const Run r = run_opd("new \"" + out.string() + "\" " + e.args);
        CHECK(r.code == 2 && r.out.empty() && r.err.find(e.line) != std::string::npos &&
                  !fs::exists(out),
              (std::string("error_") + e.name + "_exit2_exact_line_no_file").c_str());
    }

    // ================= --beats-per-bar combinations =================
    {
        const fs::path ts = scratch_dir() / "comb_ts.opj";
        const fs::path c1 = scratch_dir() / "comb_c1.opj";
        const fs::path c2 = scratch_dir() / "comb_c2.opj";
        const Run r0 = run_opd("new \"" + ts.string() + "\" --time-signature 6/8");
        const Run r1 = run_opd("new \"" + c1.string() + "\" --time-signature 6/8 --beats-per-bar 3");
        const Run r2 = run_opd("new \"" + c2.string() + "\" --beats-per-bar 3 --time-signature 6/8");
        CHECK(r0.code == 0 && r1.code == 0 && r2.code == 0 &&
                  read_file(c1) == read_file(ts) && read_file(c2) == read_file(ts),
              "consistent_beats_per_bar_equals_time_signature_alone_both_orders");
        const fs::path c3 = scratch_dir() / "comb_c3.opj";
        const fs::path c4 = scratch_dir() / "comb_c4.opj";
        const Run r3 = run_opd("new \"" + c3.string() + "\" --time-signature 3/4 --beats-per-bar 3");
        const Run r4 = run_opd("new \"" + c4.string() + "\" --time-signature 3/4");
        CHECK(r3.code == 0 && r4.code == 0 && read_file(c3) == read_file(c4),
              "consistent_beats_per_bar_with_den_4_equals_ts_alone");
    }

    // ================= last --time-signature wins; flag order independence =================
    {
        const fs::path last = scratch_dir() / "last_ts.opj";
        const fs::path one = scratch_dir() / "one_ts.opj";
        const Run r1 = run_opd("new \"" + last.string() + "\" --time-signature 6/8 --time-signature 12/16");
        const Run r2 = run_opd("new \"" + one.string() + "\" --time-signature 12/16");
        CHECK(r1.code == 0 && r2.code == 0 && read_file(last) == read_file(one),
              "repeated_time_signature_last_value_wins");
    }
    {
        const fs::path p1 = scratch_dir() / "order1.opj";
        const Run r1 = run_opd("new \"" + p1.string() + "\" --bars 7 --time-signature 6/8 --bpm 90");
        CHECK(r1.code == 0 && read_file(p1) == row0_bytes,
              "flag_order_independent_options_before_positional");
        const fs::path p2 = scratch_dir() / "order2.opj";
        const Run r2 = run_opd("new --bpm 90.0 \"" + p2.string() + "\" --time-signature 6/8 --bars 7");
        CHECK(r2.code == 0 && read_file(p2) == row0_bytes,
              "flag_order_independent_options_after_positional");
    }

    // ================= composing with --track / --sample-rate =================
    {
        const fs::path p = scratch_dir() / "track_sr.opj";
        const Run r = run_opd("new \"" + p.string() + "\" --time-signature 6/8 --sample-rate 44100 "
                              "--track Kick:sine");
        op::NewProjectSpec spec;
        spec.sample_rate = 44100;
        op::NewTrackSpec t;
        t.name = "Kick";
        t.kind = op::VoiceKind::Sine;
        spec.tracks.push_back(t);
        op::Project ref = op::new_project(spec);
        std::string err;
        const bool applied = op::apply_op(
            ref, nlohmann::json{{"op", "set_time_signature"}, {"num", 6}, {"den", 8}}, err);
        CHECK(r.code == 0 && applied && read_file(p) == op::project_bytes(ref) &&
                  r.out == "new: " + p.string() + " tracks=1 bars=4 bpm=120\n",
              "composes_with_track_and_sample_rate_in_process_reference");
    }

    // ================= existing option messages still exact =================
    {
        const fs::path out = scratch_dir() / "ex_bars.opj";
        const Run r = run_opd("new \"" + out.string() + "\" --bars 0");
        CHECK(r.code == 2 &&
                  r.err.find("error: usage: bad --bars value '0' (expected an integer 1..4096)")
                      != std::string::npos &&
                  !fs::exists(out),
              "existing_bars_0_message_unchanged");
    }
    {
        const fs::path out = scratch_dir() / "ex_bpm.opj";
        const Run r = run_opd("new \"" + out.string() + "\" --bpm 0");
        CHECK(r.code == 2 &&
                  r.err.find("error: usage: bad --bpm value '0' (expected a number in (0, 1000])")
                      != std::string::npos &&
                  !fs::exists(out),
              "existing_bpm_0_message_unchanged");
    }
    {
        const fs::path out = scratch_dir() / "ex_bpb.opj";
        const Run r = run_opd("new \"" + out.string() + "\" --beats-per-bar 33");
        CHECK(r.code == 2 &&
                  r.err.find("error: usage: bad --beats-per-bar value '33' (expected an integer 1..32)")
                      != std::string::npos &&
                  !fs::exists(out),
              "existing_beats_per_bar_33_message_unchanged");
    }
    {
        const fs::path out = scratch_dir() / "ex_sr.opj";
        const Run r = run_opd("new \"" + out.string() + "\" --sample-rate 7999");
        CHECK(r.code == 2 &&
                  r.err.find("error: usage: bad --sample-rate value '7999' (expected an integer 8000..192000)")
                      != std::string::npos &&
                  !fs::exists(out),
              "existing_sample_rate_7999_message_unchanged");
    }
    {
        // option-level errors are reported first no matter the order (the conflict check runs only
        // after the whole option loop)
        const fs::path out = scratch_dir() / "prio_a.opj";
        const Run r = run_opd("new \"" + out.string() +
                              "\" --time-signature 7/8 --beats-per-bar 33");
        CHECK(r.code == 2 &&
                  r.err.find("bad --time-signature value '7/8': bar must be a whole number of quarter-note beats")
                      != std::string::npos &&
                  !fs::exists(out),
              "option_level_error_wins_ts_left_first");
        const fs::path out2 = scratch_dir() / "prio_b.opj";
        const Run r2 = run_opd("new \"" + out2.string() +
                               "\" --beats-per-bar 33 --time-signature 3/4");
        CHECK(r2.code == 2 &&
                  r2.err.find("bad --beats-per-bar value '33' (expected an integer 1..32)")
                      != std::string::npos &&
                  !fs::exists(out2),
              "option_level_error_wins_bpb_left_first");
    }

    // ================= success line unchanged; two runs byte-identical =================
    {
        const fs::path p = scratch_dir() / "succ1.opj";
        const fs::path q = scratch_dir() / "succ2.opj";
        const Run r1 = run_opd("new \"" + p.string() + "\" --bpm 90.0 --time-signature 6/8 --bars 7");
        const Run r2 = run_opd("new \"" + q.string() + "\" --bpm 90.0 --time-signature 6/8 --bars 7");
        CHECK(r1.code == 0 &&
                  r1.out == "new: " + p.string() + " tracks=1 bars=7 bpm=90\n",
              "success_line_unchanged_no_signature");
        CHECK(r2.code == 0 && read_file(p) == read_file(q),
              "two_identical_runs_byte_identical");
    }

    fs::remove_all(scratch_dir(), ec);
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
