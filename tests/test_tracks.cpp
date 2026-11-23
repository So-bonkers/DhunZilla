// Unit tests for op::tracks_json (WP-0227, docs/spec/operations.md §Channel rack / mixer track view): the
// read-only channel-rack / mixer track view — one entry per track in model order, audibility from
// op::track_audible, voices byte-identical to the canonical project serializer. Boring assert-based runner
// — no framework deps, own main. Exit code = number of failures. Projects live in one per-process scratch
// dir (removed at the end); CLI checks run the real opd binary via OPD_BIN.
// Run: ./build/op_tracks_tests
#include <unistd.h>  // getpid

#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "engine/model.hpp"
#include "engine/tracks.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                              \
    do {                                                                               \
        if (cond) std::printf("PASS %s\n", name);                                      \
        else { std::printf("FAIL %s  (%s:%d)\n", name, __FILE__, __LINE__); ++g_failures; } \
    } while (0)

namespace fs = std::filesystem;

static const fs::path& scratch_dir() {
    static const fs::path dir = fs::temp_directory_path() /
        ("op_tracks_tests_wp0227_" + std::to_string(static_cast<long>(::getpid())));  // per-process, no collisions
    return dir;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------- opd runner (style of test_audition.cpp)

struct Run {
    int code = -1;
    std::string out;  // stdout
    std::string err;  // stderr (captured to a scratch file, so one run per call)
};

static Run run_opd(const std::string& args) {  // generic: args start with the subcommand ("tracks ...")
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

static bool has_exact_keys(const nlohmann::json& o, std::initializer_list<const char*> want) {
    if (!o.is_object() || o.size() != want.size()) return false;
    for (const char* k : want)
        if (!o.contains(k)) return false;
    return true;
}

// audible equals op::track_audible for EVERY track of the project (the view never re-derives the rule).
static bool audible_matches_rule(const op::Project& p) {
    const nlohmann::json arr = nlohmann::json::parse(op::tracks_json(p));
    if (!arr.is_array() || arr.size() != p.tracks.size()) return false;
    for (std::size_t i = 0; i < p.tracks.size(); ++i)
        if (arr[i].at("audible").get<bool>() != op::track_audible(p, i)) return false;
    return true;
}

// 120 bpm, 4/4, 48 kHz. The view reads only tracks, so most projects need no patterns at all.
static op::Project base_project() {
    op::Project p;
    p.format_version = 0;
    p.bpm = 120.0;
    p.sample_rate = 48000;
    p.seed = 42;
    p.length_bars = 4.0;
    p.beats_per_bar = 4;
    return p;
}

// Three plain sine tracks with the given mute/solo flags (names t0..t2).
static op::Project mixer_project(bool m0, bool s0, bool m1, bool s1, bool m2, bool s2) {
    op::Project p = base_project();
    for (int i = 0; i < 3; ++i) {
        op::Track t;
        t.name = "t" + std::to_string(i);
        t.voice.kind = op::VoiceKind::Sine;
        p.tracks.push_back(t);
    }
    p.tracks[0].mute = m0;  p.tracks[0].solo = s0;
    p.tracks[1].mute = m1;  p.tracks[1].solo = s1;
    p.tracks[2].mute = m2;  p.tracks[2].solo = s2;
    return p;
}

// The spec's worked example: soloed sampler with trim + root note + choke group 2, muted synth v1 saw
// with ADSR and cutoff (resonance at default → omitted), metronome at defaults.
static op::Project spec_example_project() {
    op::Project p = base_project();
    {
        op::Track t;
        t.name = "Zeta Kick";
        t.gain_db = -3.5f;
        t.pan = 0.25f;
        t.solo = true;
        t.choke_group = 2;
        t.voice.kind = op::VoiceKind::Sampler;
        t.voice.path = "kits/kick.wav";
        t.voice.start_s = 0.0625;
        t.voice.has_end_s = true;
        t.voice.end_s = 0.5;
        t.voice.root_note = 60;
        p.tracks.push_back(t);
    }
    {
        op::Track t;
        t.name = "Alpha Lead";
        t.gain_db = -6.0f;
        t.pan = -0.5f;
        t.mute = true;
        t.voice.kind = op::VoiceKind::Synth;
        t.voice.waveform = op::Waveform::Saw;
        t.voice.has_adsr = true;
        t.voice.attack_s = 0.01;
        t.voice.decay_s = 0.25;
        t.voice.sustain = 0.5;
        t.voice.release_s = 0.125;
        t.voice.cutoff_hz = 2500.0;  // resonance stays at its default 0.7071 → omitted
        p.tracks.push_back(t);
    }
    {
        op::Track t;
        t.name = "Click";
        t.voice.kind = op::VoiceKind::Metronome;
        p.tracks.push_back(t);
    }
    return p;
}

int main() {
    std::error_code ec;
    fs::remove_all(scratch_dir(), ec);
    fs::create_directories(scratch_dir(), ec);
    if (ec) {
        std::printf("FATAL: cannot create temp dir %s\n", scratch_dir().string().c_str());
        return 2;
    }

    // --- empty-tracks Project (only constructible in memory) → exactly "[]\n".
    {
        const op::Project p = base_project();  // zero tracks
        CHECK(op::tracks_json(p) == "[]\n", "tracks.empty_project_prints_exactly_empty_array");
    }

    // --- the spec's byte-exact example (all three entries), and exactly one trailing '\n'.
    {
        const std::string want = R"JSON([
  {
    "audible": true,
    "choke_group": 2,
    "gain_db": -3.5,
    "index": 0,
    "kind": "sampler",
    "mute": false,
    "name": "Zeta Kick",
    "pan": 0.25,
    "solo": true,
    "voice": {
      "end_s": 0.5,
      "kind": "sampler",
      "path": "kits/kick.wav",
      "root_note": 60,
      "start_s": 0.0625
    }
  },
  {
    "audible": false,
    "choke_group": 0,
    "gain_db": -6.0,
    "index": 1,
    "kind": "synth",
    "mute": true,
    "name": "Alpha Lead",
    "pan": -0.5,
    "solo": false,
    "voice": {
      "adsr": {
        "attack_s": 0.01,
        "decay_s": 0.25,
        "release_s": 0.125,
        "sustain": 0.5
      },
      "attack_ms": 5.0,
      "cutoff_hz": 2500.0,
      "kind": "synth",
      "release_ms": 30.0,
      "waveform": "saw"
    }
  },
  {
    "audible": false,
    "choke_group": 0,
    "gain_db": 0.0,
    "index": 2,
    "kind": "metronome",
    "mute": false,
    "name": "Click",
    "pan": 0.0,
    "solo": false,
    "voice": {
      "accent_every": 4,
      "kind": "metronome"
    }
  }
]
)JSON";
        const std::string s = op::tracks_json(spec_example_project());
        CHECK(s == want, "tracks.spec_example_byte_exact");
        CHECK(s.size() >= 3 && s[s.size() - 1] == '\n' && s[s.size() - 2] != '\n',
              "tracks.exactly_one_trailing_newline");
    }

    // --- model order preserved with deliberately non-alphabetical names; index values are 0..n-1.
    {
        op::Project p = base_project();
        for (const char* n : {"Zulu", "Mike", "Alpha"}) {
            op::Track t;
            t.name = n;
            t.voice.kind = op::VoiceKind::Sine;
            p.tracks.push_back(t);
        }
        const std::string s = op::tracks_json(p);
        CHECK(s.find("\"name\": \"Zulu\"") < s.find("\"name\": \"Mike\"") &&
                  s.find("\"name\": \"Mike\"") < s.find("\"name\": \"Alpha\""),
              "tracks.model_order_never_sorted_by_name");
        const nlohmann::json arr = nlohmann::json::parse(s);
        bool idx_ok = arr.size() == 3;
        for (std::size_t i = 0; i < arr.size(); ++i)
            idx_ok = idx_ok && arr[i].at("index").is_number_integer() &&
                     arr[i].at("index").get<std::size_t>() == i;
        CHECK(idx_ok, "tracks.index_values_are_0_to_n_minus_1_integers");
    }

    // --- audibility matrix (the renderer's own rule via op::track_audible).
    {
        // No solo: every un-muted track audible, the muted one not.
        const op::Project no_solo = mixer_project(false, false, true, false, false, false);
        const nlohmann::json a1 = nlohmann::json::parse(op::tracks_json(no_solo));
        CHECK(a1[0].at("audible").get<bool>() && !a1[1].at("audible").get<bool>() &&
                  a1[2].at("audible").get<bool>(),
              "tracks.audible_no_solo_mute_is_the_only_filter");

        // One solo: only it is audible, even though the others are un-muted.
        const op::Project one_solo = mixer_project(false, true, false, false, false, false);
        const nlohmann::json a2 = nlohmann::json::parse(op::tracks_json(one_solo));
        CHECK(a2[0].at("audible").get<bool>() && !a2[1].at("audible").get<bool>() &&
                  !a2[2].at("audible").get<bool>(),
              "tracks.audible_one_solo_only_the_solo");

        // Two solo of which one is muted: only the unmuted solo; mute wins over its own solo.
        const op::Project two_solo = mixer_project(false, true, true, true, false, false);
        const nlohmann::json a3 = nlohmann::json::parse(op::tracks_json(two_solo));
        CHECK(a3[0].at("audible").get<bool>() && !a3[1].at("audible").get<bool>() &&
                  !a3[2].at("audible").get<bool>(),
              "tracks.audible_muted_and_soloed_not_audible_mute_wins");

        // All tracks soloed: every un-muted one is audible.
        const op::Project all_solo = mixer_project(false, true, false, true, true, true);
        const nlohmann::json a4 = nlohmann::json::parse(op::tracks_json(all_solo));
        CHECK(a4[0].at("audible").get<bool>() && a4[1].at("audible").get<bool>() &&
                  !a4[2].at("audible").get<bool>(),
              "tracks.audible_all_soloed_unmuted_audible");

        // audible equals op::track_audible for every track of every matrix project (never re-derived).
        CHECK(audible_matches_rule(no_solo) && audible_matches_rule(one_solo) &&
                  audible_matches_rule(two_solo) && audible_matches_rule(all_solo) &&
                  audible_matches_rule(spec_example_project()),
              "tracks.audible_equals_track_audible_on_every_matrix_project");
    }

    // --- the ten entry keys all present at defaults; default mixer fields print as themselves.
    {
        op::Project p = base_project();
        op::Track t;
        t.name = "Default";
        p.tracks.push_back(t);  // everything at its default
        const std::string s = op::tracks_json(p);
        const nlohmann::json arr = nlohmann::json::parse(s);
        CHECK(has_exact_keys(arr[0], {"audible", "choke_group", "gain_db", "index", "kind",
                                      "mute", "name", "pan", "solo", "voice"}),
              "tracks.exactly_ten_entry_keys_at_defaults");
        CHECK(s.find("\"mute\": false") != std::string::npos &&
                  s.find("\"solo\": false") != std::string::npos &&
                  s.find("\"choke_group\": 0") != std::string::npos &&
                  s.find("\"gain_db\": 0.0") != std::string::npos &&
                  s.find("\"pan\": 0.0") != std::string::npos,
              "tracks.default_mixer_fields_print_as_themselves");
    }

    // --- sampler voice: optional keys the model does not hold are ABSENT (never -1 / null / defaults).
    {
        op::Project p = base_project();
        op::Track t;
        t.name = "S";
        t.voice.kind = op::VoiceKind::Sampler;
        t.voice.path = "kits/snare.wav";  // no trim, no root note
        p.tracks.push_back(t);
        const std::string s = op::tracks_json(p);
        CHECK(s.find("start_s") == std::string::npos && s.find("end_s") == std::string::npos &&
                  s.find("root_note") == std::string::npos && s.find("-1") == std::string::npos,
              "tracks.sampler_optional_keys_absent_never_minus_1");
        const nlohmann::json arr = nlohmann::json::parse(s);
        CHECK(has_exact_keys(arr[0].at("voice"), {"kind", "path"}),
              "tracks.sampler_default_voice_exactly_kind_and_path");
    }
    {
        op::Project p = base_project();
        op::Track t;
        t.name = "S";
        t.voice.kind = op::VoiceKind::Sampler;
        t.voice.path = "kits/snare.wav";
        t.voice.start_s = 1.5;
        t.voice.has_end_s = true;
        t.voice.end_s = 3.5;
        t.voice.root_note = 48;
        p.tracks.push_back(t);
        const std::string s = op::tracks_json(p);
        CHECK(s.find("\"start_s\": 1.5") != std::string::npos &&
                  s.find("\"end_s\": 3.5") != std::string::npos &&
                  s.find("\"root_note\": 48") != std::string::npos,
              "tracks.sampler_trim_and_root_present_when_set");
    }

    // --- legacy synth voice: only kind / attack_ms / release_ms (no waveform/adsr/cutoff_hz/resonance).
    {
        op::Project p = base_project();
        op::Track t;
        t.name = "Old";
        t.voice.kind = op::VoiceKind::Synth;  // Waveform::Legacy, no ADSR, cutoff 0, resonance default
        p.tracks.push_back(t);
        const std::string s = op::tracks_json(p);
        CHECK(s.find("waveform") == std::string::npos && s.find("adsr") == std::string::npos &&
                  s.find("cutoff_hz") == std::string::npos && s.find("resonance") == std::string::npos,
              "tracks.legacy_synth_has_no_v1_keys");
        const nlohmann::json arr = nlohmann::json::parse(s);
        CHECK(has_exact_keys(arr[0].at("voice"), {"kind", "attack_ms", "release_ms"}),
              "tracks.legacy_synth_voice_exactly_kind_attack_release");
    }

    // --- synth v1 with ADSR + cutoff, resonance at its default → resonance ABSENT, everything else present.
    {
        op::Project p = base_project();
        op::Track t;
        t.name = "V1";
        t.voice.kind = op::VoiceKind::Synth;
        t.voice.waveform = op::Waveform::Square;
        t.voice.has_adsr = true;  // all four inner keys, defaults included
        t.voice.cutoff_hz = 500.0;
        p.tracks.push_back(t);
        const std::string s = op::tracks_json(p);
        CHECK(s.find("resonance") == std::string::npos,
              "tracks.synth_v1_default_resonance_absent");
        CHECK(s.find("\"waveform\": \"square\"") != std::string::npos &&
                  s.find("\"adsr\"") != std::string::npos &&
                  s.find("\"cutoff_hz\": 500.0") != std::string::npos,
              "tracks.synth_v1_waveform_adsr_cutoff_present");
        const nlohmann::json arr = nlohmann::json::parse(s);
        CHECK(has_exact_keys(arr[0].at("voice").at("adsr"), {"attack_s", "decay_s", "sustain", "release_s"}),
              "tracks.adsr_always_carries_all_four_inner_keys");
    }

    // --- non-default resonance → present.
    {
        op::Project p = base_project();
        op::Track t;
        t.name = "Res";
        t.voice.kind = op::VoiceKind::Synth;
        t.voice.waveform = op::Waveform::Saw;
        t.voice.resonance = 2.5;
        p.tracks.push_back(t);
        const std::string s = op::tracks_json(p);
        CHECK(s.find("\"resonance\": 2.5") != std::string::npos,
              "tracks.synth_nondefault_resonance_present");
    }

    // --- metronome / sine voices report exactly their own keys.
    {
        op::Project p = base_project();
        op::Track m;
        m.name = "Metro";
        m.voice.kind = op::VoiceKind::Metronome;
        op::Track s;
        s.name = "Tone";
        s.voice.kind = op::VoiceKind::Sine;
        p.tracks.push_back(m);
        p.tracks.push_back(s);
        const nlohmann::json arr = nlohmann::json::parse(op::tracks_json(p));
        CHECK(has_exact_keys(arr[0].at("voice"), {"kind", "accent_every"}),
              "tracks.metronome_voice_exactly_kind_and_accent_every");
        CHECK(has_exact_keys(arr[1].at("voice"), {"kind", "freq_hz"}),
              "tracks.sine_voice_exactly_kind_and_freq_hz");
    }

    // --- track name JSON escaping (quote / backslash / newline): escaped in text, exact on parse-back.
    {
        const std::string tricky = std::string("q\"b\\c") + '\n' + "nl";
        op::Project p = base_project();
        op::Track t;
        t.name = tricky;
        t.voice.kind = op::VoiceKind::Sine;
        p.tracks.push_back(t);
        const std::string s = op::tracks_json(p);
        CHECK(s.find("\\\"") != std::string::npos && s.find("\\\\") != std::string::npos &&
                  s.find("\\n") != std::string::npos,
              "tracks.name_escaping_quote_backslash_newline_in_text");
        const nlohmann::json arr = nlohmann::json::parse(s);
        CHECK(arr[0].at("name").get<std::string>() == tricky, "tracks.name_escapes_round_trip_via_parse");
    }

    // --- two calls on the same Project are byte-identical (pure function).
    {
        const op::Project p = spec_example_project();
        CHECK(op::tracks_json(p) == op::tracks_json(p), "tracks.two_calls_byte_identical");
    }

    // --- save/load round trip through a scratch .opj, then the real binary via OPD_BIN: stdout equals
    // tracks_json(project); the saved file bytes stay unchanged; two runs are byte-identical.
    {
        const fs::path opj = scratch_dir() / "example.opj";
        std::string err;
        const op::Project p = spec_example_project();
        CHECK(op::save_project(opj.string(), p, err), "tracks.cli_save_project_succeeds");
        const std::string bytes_before = read_file(opj);

        op::Project loaded;
        CHECK(op::load_project(opj.string(), loaded, err) &&
                  op::tracks_json(loaded) == op::tracks_json(p),
              "tracks.save_load_roundtrip_output_stable");

        const Run r1 = run_opd("tracks \"" + opj.string() + "\"");
        CHECK(r1.code == 0 && r1.out == op::tracks_json(p), "tracks.cli_exit_0_stdout_equals_tracks_json");
        const Run r2 = run_opd("tracks \"" + opj.string() + "\"");
        CHECK(r1.out == r2.out, "tracks.cli_two_runs_byte_identical");
        CHECK(read_file(opj) == bytes_before, "tracks.cli_project_file_bytes_unchanged");
    }

    // --- CLI creates no files: a dedicated subdir holds exactly the .opj before and after the run.
    {
        const fs::path sub = scratch_dir() / "noio";
        fs::create_directories(sub, ec);
        const fs::path opj = sub / "one.opj";
        std::string err;
        op::Project p = base_project();
        op::Track t;
        t.name = "One";
        t.voice.kind = op::VoiceKind::Sine;
        p.tracks.push_back(t);
        CHECK(op::save_project(opj.string(), p, err), "tracks.noio_save_project_succeeds");
        const Run r = run_opd("tracks \"" + opj.string() + "\"");
        std::size_t entries = 0;
        for (fs::directory_iterator it(sub, ec); !ec && it != fs::directory_iterator(); ++it) ++entries;
        CHECK(r.code == 0 && entries == 1, "tracks.cli_creates_no_files");
    }

    // --- CLI errors: usage → exit 2 with empty stdout; missing file → exit 3; garbage → exit 4.
    {
        const Run r = run_opd("tracks");
        CHECK(r.code == 2 && r.out.empty() &&
                  r.err.find("tracks needs exactly <project.opj>") != std::string::npos,
              "tracks.cli_no_argument_exit_2_empty_stdout");
    }
    {
        const fs::path opj = scratch_dir() / "example.opj";
        const Run r = run_opd("tracks \"" + opj.string() + "\" extra.opj");
        CHECK(r.code == 2 && r.out.empty() &&
                  r.err.find("tracks needs exactly <project.opj>") != std::string::npos,
              "tracks.cli_extra_argument_exit_2_empty_stdout");
    }
    {
        const fs::path opj = scratch_dir() / "example.opj";
        const Run r = run_opd("tracks --json \"" + opj.string() + "\"");
        CHECK(r.code == 2 && r.out.empty() &&
                  r.err.find("tracks needs exactly <project.opj>") != std::string::npos,
              "tracks.cli_option_token_exit_2_empty_stdout");
    }
    {
        const std::string missing = (scratch_dir() / "nope.opj").string();
        const Run r = run_opd("tracks \"" + missing + "\"");
        CHECK(r.code == 3 && r.out.empty() &&
                  r.err.find("not_found") != std::string::npos &&
                  r.err.find(missing) != std::string::npos,
              "tracks.cli_missing_file_exit_3_detail_is_path");
    }
    {
        const fs::path bad = scratch_dir() / "garbage.opj";
        {
            std::ofstream f(bad, std::ios::binary);
            f << "{ this is not json at all\n";
        }
        const Run r = run_opd("tracks \"" + bad.string() + "\"");
        CHECK(r.code == 4 && r.out.empty() && r.err.find("parse") != std::string::npos,
              "tracks.cli_garbage_file_exit_4_parse");
    }

    fs::remove_all(scratch_dir(), ec);
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
