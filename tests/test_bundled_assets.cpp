// Unit tests for the bundled generated content (WP-0215): assets/kits/*.wav + assets/presets/
// synth_presets.json + assets/manifest.json. Regenerates the whole tree twice with
// tools/gen_bundled_kit.py into a per-process scratch dir and compares byte-for-byte with the
// committed tree; checks manifest keys/licence/note/file list, every kit WAV through
// op::wav::read_wav (format, exact length, peak headroom, silent ends, early attack), pairwise
// distinct kit files, the exact preset shape, and that op::apply_op accepts all four
// set_synth_params presets on a one-synth-track project. Boring assert-based runner — no
// framework deps, own main. Exit code = number of failures.
// Run: ./build/op_bundled_tests
#include <unistd.h>  // getpid

#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/model.hpp"
#include "engine/ops.hpp"
#include "engine/wav.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;

static const fs::path& scratch_dir() {
    static const fs::path dir = fs::temp_directory_path() /
        ("op_bundled_tests_wp0215_" + std::to_string(static_cast<long>(::getpid())));  // per-process, no collisions
    return dir;
}

static const fs::path& assets_dir() {  // OP_TEST_DATA is "<repo>/tests"; the repo root is its parent
    static const fs::path dir = fs::path(OP_TEST_DATA).parent_path() / "assets";
    return dir;
}

static std::string read_file(const fs::path& p, bool* ok = nullptr) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ok) *ok = f.good() || f.eof();
    return ss.str();
}

// Relative (generic_string → '/' separators), sorted list of every regular file under root.
static std::vector<std::string> list_files(const fs::path& root) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file())
            out.push_back(fs::relative(e.path(), root).generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

static bool has_exact_keys(const nlohmann::json& o, std::initializer_list<const char*> want) {
    if (!o.is_object() || o.size() != want.size()) return false;
    for (const char* k : want)
        if (!o.contains(k)) return false;
    return true;
}

static bool is_lower_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

struct Run {
    int code = -1;
    std::string out;  // stdout (stderr redirected to /dev/null in the command)
};

static Run run_cmd(const std::string& cmd) {
    Run r;
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return r;
    char buf[256];
    while (std::fgets(buf, sizeof buf, pipe)) r.out += buf;
    const int status = ::pclose(pipe);
    r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return r;
}

// The WP-0215 table: kit file → exact frame count = round(sec × 48000).
struct KitSpec {
    const char* file;
    std::size_t frames;
};
static const KitSpec kKits[] = {
    {"kick.wav", 21600},         {"snare.wav", 16800},         {"clap.wav", 19200},
    {"hat_closed.wav", 3840},   {"hat_open.wav", 24000},      {"rim.wav", 5760},
    {"tom.wav", 26400},         {"perc_cowbell.wav", 14400},  {"perc_clave.wav", 4800},
};

static double max_abs_abs(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double m = 0.0;
    for (std::size_t i = from; i < to; ++i)
        m = std::max(m, static_cast<double>(std::abs(x[i])));
    return m;
}

int main() {
    std::error_code ec;
    fs::remove_all(scratch_dir(), ec);
    const fs::path gen1 = scratch_dir() / "gen1";
    const fs::path gen2 = scratch_dir() / "gen2";

    // ---------------------------------------------------------------- regenerate twice (byte-for-byte)
    const fs::path script = fs::path(OP_TEST_DATA).parent_path() / "tools" / "gen_bundled_kit.py";
    CHECK(fs::is_directory(assets_dir()), "committed.assets_tree_exists");
    Run r1 = run_cmd("python3 \"" + script.string() + "\" --out \"" + gen1.string() + "\" 2>/dev/null");
    Run r2 = run_cmd("python3 \"" + script.string() + "\" --out \"" + gen2.string() + "\" 2>/dev/null");
    CHECK(r1.code == 0, "gen.regen1_exit_0");
    CHECK(r2.code == 0, "gen.regen2_exit_0");
    CHECK(r1.out.rfind("gen_bundled_kit: wrote ", 0) == 0, "gen.regen1_stdout_line");
    CHECK(r2.out.rfind("gen_bundled_kit: wrote ", 0) == 0, "gen.regen2_stdout_line");

    const std::vector<std::string> a1 = list_files(gen1);
    const std::vector<std::string> a2 = list_files(gen2);
    const std::vector<std::string> com = list_files(assets_dir());
    CHECK(a1 == a2, "gen.regen1_and_regen2_file_sets_match");
    CHECK(a1 == com, "gen.regen_file_set_equals_committed_tree");
    CHECK(com.size() == 11, "gen.committed_tree_has_exactly_11_files");
    const std::vector<std::string> expected = {
        "kits/clap.wav", "kits/hat_closed.wav", "kits/hat_open.wav", "kits/kick.wav",
        "kits/perc_clave.wav", "kits/perc_cowbell.wav", "kits/rim.wav", "kits/snare.wav",
        "kits/tom.wav", "manifest.json", "presets/synth_presets.json"};
    CHECK(com == expected, "gen.committed_tree_matches_the_11_listed_files");

    for (const std::string& rel : expected) {
        bool ok_c = false, ok_1 = false, ok_2 = false;
        const std::string c = read_file(assets_dir() / rel, &ok_c);
        const std::string g1 = read_file(gen1 / rel, &ok_1);
        const std::string g2 = read_file(gen2 / rel, &ok_2);
        CHECK(ok_c && ok_1 && ok_2 && !c.empty() && g1 == c && g2 == c,
              ("regen.byte_identical." + rel).c_str());
    }

    // ---------------------------------------------------------------- manifest.json
    const std::string manifest_text = read_file(assets_dir() / "manifest.json");
    const nlohmann::json man = nlohmann::json::parse(manifest_text, nullptr, false);
    CHECK(!man.is_discarded(), "manifest.parse_ok");
    if (!man.is_discarded()) {
        CHECK(has_exact_keys(man, {"format_version", "generator", "seed", "sample_rate",
                                   "license", "note", "files"}),
              "manifest.exactly_seven_top_level_keys");
        CHECK(man.value("format_version", -1) == 0, "manifest.format_version_0");
        CHECK(man.value("generator", "") == "tools/gen_bundled_kit.py", "manifest.generator");
        CHECK(man.contains("seed") && man["seed"].is_number_integer() &&
                  man["seed"].get<long long>() > 0,
              "manifest.seed_is_a_positive_integer");
        CHECK(man.value("sample_rate", 0) == 48000, "manifest.sample_rate_48000");
        CHECK(man.value("license", "") == "CC0-1.0", "manifest.license_cc0");
        CHECK(man.value("note", "") ==
                  "generated in-repo by tools/gen_bundled_kit.py; no third-party samples",
              "manifest.licence_note_verbatim");
        std::set<std::string> listed, on_disk;
        bool all_sha_hex = false;
        if (man.contains("files") && man["files"].is_object()) {
            all_sha_hex = true;
            for (const auto& [rel, h] : man["files"].items()) {
                listed.insert(rel);
                if (!h.is_string() || !is_lower_hex64(h.get<std::string>())) all_sha_hex = false;
            }
        }
        for (const std::string& rel : com)
            if (rel != "manifest.json") on_disk.insert(rel);
        CHECK(listed == on_disk, "manifest.lists_exactly_the_files_on_disk");
        CHECK(all_sha_hex && !listed.empty(), "manifest.all_sha256_are_lowercase_hex64");
    }

    // ---------------------------------------------------------------- every kit WAV
    for (const KitSpec& k : kKits) {
        const std::string path = (assets_dir() / "kits" / k.file).string();
        const op::wav::ReadResult rr = op::wav::read_wav(path);
        const std::string n = k.file;
        CHECK(rr.ok, ("kit." + n + ".read_wav_ok").c_str());
        if (!rr.ok) { ++g_failures; continue; }  // every remaining per-kit CHECK would be vacuous
        CHECK(rr.channels == 1 && rr.sample_rate == 48000, ("kit." + n + ".mono_48k").c_str());
        CHECK(rr.samples.size() == k.frames, ("kit." + n + ".exact_frames_" + std::to_string(k.frames)).c_str());
        if (rr.samples.size() < 480) continue;
        const double peak = max_abs_abs(rr.samples, 0, rr.samples.size());
        CHECK(peak >= 0.68 && peak <= 0.71, ("kit." + n + ".peak_0.68_0.71_below_minus_1dbfs").c_str());
        CHECK(rr.samples.front() == 0.0f && rr.samples.back() == 0.0f, ("kit." + n + ".first_and_last_sample_exactly_0").c_str());
        CHECK(max_abs_abs(rr.samples, rr.samples.size() - 8, rr.samples.size()) <= 0.05,
              ("kit." + n + ".last8_max_le_0.05_fadeout").c_str());
        CHECK(max_abs_abs(rr.samples, 1, 4) <= 0.10, ("kit." + n + ".frames_1_3_max_le_0.10_no_onset_click").c_str());
        CHECK(max_abs_abs(rr.samples, 0, 480) >= 0.20, ("kit." + n + ".first480_max_ge_0.20_early_attack").c_str());
    }

    // Pairwise distinct (no duplicated sample under two names) and not silence.
    bool kits_distinct = true;
    for (std::size_t i = 0; i < sizeof kKits / sizeof kKits[0]; ++i) {
        const std::string bi = read_file(assets_dir() / "kits" / kKits[i].file);
        if (bi.empty()) { kits_distinct = false; continue; }
        for (std::size_t j = i + 1; j < sizeof kKits / sizeof kKits[0]; ++j) {
            const std::string bj = read_file(assets_dir() / "kits" / kKits[j].file);
            if (bi == bj) kits_distinct = false;
        }
    }
    CHECK(kits_distinct, "kits.all_9_files_pairwise_distinct_and_not_empty");

    // ---------------------------------------------------------------- presets JSON shape
    const std::string presets_text = read_file(assets_dir() / "presets" / "synth_presets.json");
    const nlohmann::json pre = nlohmann::json::parse(presets_text, nullptr, false);
    CHECK(!pre.is_discarded(), "presets.parse_ok");
    nlohmann::json presets = nlohmann::json::array();
    if (!pre.is_discarded()) {
        CHECK(has_exact_keys(pre, {"format_version", "generator", "license", "note", "presets"}),
              "presets.exactly_five_top_level_keys");
        CHECK(pre.value("format_version", -1) == 0, "presets.format_version_0");
        CHECK(pre.value("generator", "") == "tools/gen_bundled_kit.py", "presets.generator");
        CHECK(pre.value("license", "") == "CC0-1.0", "presets.license_cc0");
        CHECK(pre.value("note", "") ==
                  "generated in-repo by tools/gen_bundled_kit.py; no third-party samples",
              "presets.licence_note_verbatim");
        if (pre.contains("presets") && pre["presets"].is_array()) presets = pre["presets"];
    }
    CHECK(presets.size() == 4, "presets.exactly_four_presets");
    {
        bool names_ok = presets.size() == 4;
        const char* want[] = {"bass", "lead", "pad", "pluck"};
        for (std::size_t i = 0; i < presets.size() && i < 4; ++i)
            if (!presets[i].is_object() || presets[i].value("name", "") != want[i]) names_ok = false;
        CHECK(names_ok, "presets.names_are_bass_lead_pad_pluck_in_order");
        bool shape_ok = presets.size() == 4;
        for (const auto& p : presets) {
            if (!has_exact_keys(p, {"args", "name", "op"})) shape_ok = false;
            else if (p.value("op", "") != "set_synth_params") shape_ok = false;
            else if (!has_exact_keys(p["args"], {"adsr", "cutoff_hz", "resonance", "waveform"})) shape_ok = false;
            else if (!has_exact_keys(p["args"]["adsr"], {"attack_s", "decay_s", "release_s", "sustain"})) shape_ok = false;
        }
        CHECK(shape_ok, "presets.every_preset_has_exactly_args_name_op_and_full_arg_keys");
        bool ranges_ok = presets.size() == 4;
        for (const auto& p : presets) {
            if (!p.is_object() || !p["args"].is_object()) { ranges_ok = false; continue; }
            const auto& a = p["args"];
            const std::string wf = a["waveform"].is_string() ? a["waveform"].get<std::string>() : "";
            if (wf != "sine" && wf != "saw" && wf != "square") ranges_ok = false;
            const double co = a["cutoff_hz"].is_number() ? a["cutoff_hz"].get<double>() : -1.0;
            const double re = a["resonance"].is_number() ? a["resonance"].get<double>() : -1.0;
            if (!(co >= 20.0 && co <= 20000.0) || !(re >= 0.5 && re <= 20.0)) ranges_ok = false;
            const auto& ad = a["adsr"].is_object() ? a["adsr"] : nlohmann::json::object();
            const double at = ad.value("attack_s", -1.0), dc = ad.value("decay_s", -1.0);
            const double rs = ad.value("release_s", -1.0), su = ad.value("sustain", -1.0);
            if (!(at >= 0.0 && at <= 10.0) || !(dc >= 0.0 && dc <= 10.0) ||
                !(rs >= 0.0 && rs <= 10.0) || !(su >= 0.0 && su <= 1.0)) ranges_ok = false;
        }
        CHECK(ranges_ok, "presets.every_value_is_inside_the_synth_v1_ranges");
    }

    // ---------------------------------------------------------------- set_synth_params acceptance
    for (const auto& p : presets) {
        if (!p.is_object() || !p["args"].is_object() || !p["args"]["adsr"].is_object() ||
            !p["args"]["waveform"].is_string() || !p["args"]["cutoff_hz"].is_number() ||
            !p["args"]["resonance"].is_number()) { ++g_failures; continue; }
        const std::string name = p.value("name", "?");
        nlohmann::json opj = p["args"];          // the preset's args verbatim…
        opj["op"] = "set_synth_params";          // …as one journal operation on track 0
        opj["index"] = 0;

        op::Project proj;                        // one-synth-track project
        proj.format_version = 0;
        proj.bpm = 120.0;
        proj.sample_rate = 48000;
        proj.seed = 42;
        proj.length_bars = 4.0;
        proj.beats_per_bar = 4;
        op::Track t;
        t.name = "S";
        t.voice.kind = op::VoiceKind::Synth;
        proj.tracks.push_back(t);

        std::string err;
        CHECK(op::apply_op(proj, opj, err), ("preset." + name + ".apply_op_ok").c_str());
        const op::Voice& v = proj.tracks[0].voice;
        const auto& a = p["args"];
        const std::string wf = a["waveform"].get<std::string>();
        op::Waveform want = op::Waveform::Legacy;
        if (wf == "sine") want = op::Waveform::Sine;
        else if (wf == "saw") want = op::Waveform::Saw;
        else if (wf == "square") want = op::Waveform::Square;
        CHECK(v.waveform == want, ("preset." + name + ".voice_waveform").c_str());
        CHECK(v.cutoff_hz == a["cutoff_hz"].get<double>() &&
                  v.resonance == a["resonance"].get<double>(),
              ("preset." + name + ".voice_cutoff_and_resonance").c_str());
        const auto& ad = a["adsr"];
        CHECK(v.has_adsr &&
                  v.attack_s == ad["attack_s"].get<double>() &&
                  v.decay_s == ad["decay_s"].get<double>() &&
                  v.release_s == ad["release_s"].get<double>() &&
                  v.sustain == ad["sustain"].get<double>(),
              ("preset." + name + ".voice_adsr_all_four").c_str());
    }

    fs::remove_all(scratch_dir(), ec);
    if (g_failures == 0) std::printf("ALL PASS (0 failures)\n");
    else std::printf("%d FAILURES\n", g_failures);
    return g_failures;
}
