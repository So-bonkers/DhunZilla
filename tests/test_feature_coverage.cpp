// Unit tests for op::feature_coverage_json (WP-0211, docs/spec/golden-coverage.md): the golden feature-
// coverage report — the exact 32-row table in table order, per-golden feature lists, the uncovered set,
// name-ascending sorting regardless of input order, purity/byte-identity — plus the `opd golden-coverage`
// CLI contract (exit codes, empty stdout on errors, read-only, byte-identical runs) through the real opd
// binary via OPD_BIN. Boring assert-based runner — no framework deps, own main. Exit code = number of
// failures. Projects live in one per-process scratch dir (removed at the end).
// Run: ./build/op_feature_coverage_tests
#include <unistd.h>  // getpid

#include <sys/wait.h>  // WIFEXITED / WEXITSTATUS

#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/feature_coverage.hpp"
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
        ("op_feature_coverage_wp0211_" + std::to_string(static_cast<long>(::getpid())));  // per-process, no collisions
    return dir;
}

static std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------- opd runner (style of test_tracks.cpp)

struct Run {
    int code = -1;
    std::string out;  // stdout
    std::string err;  // stderr (captured to a scratch file, so one run per call)
};

static Run run_opd(const std::string& args) {  // generic: args start with the subcommand ("golden-coverage ...")
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

// ---------------------------------------------------------------- the feature table as the test knows it

// Exactly the docs/spec/golden-coverage.md rows, ASCII-ascending keys = table order.
static const char* const kKeys[] = {
    "arrangement",
    "attachments",
    "beats_per_bar_not_4",
    "choke_group",
    "clip_fade_in",
    "clip_fade_out",
    "clip_gain_db",
    "clip_length_beats",
    "clip_offset_beats",
    "clip_repeats",
    "loop_region",
    "master_gain_db",
    "metronome_accent",
    "mute",
    "note_pitch",
    "note_velocity",
    "pan",
    "pattern_id",
    "pattern_length_beats",
    "sample_rate_not_48k",
    "sampler_repitch",
    "sampler_slice",
    "solo",
    "swing",
    "synth_v1",
    "tempo_map",
    "time_signature",
    "track_gain_db",
    "voice_kind_metronome",
    "voice_kind_sampler",
    "voice_kind_sine",
    "voice_kind_synth",
};
static constexpr std::size_t kKeyCount = sizeof kKeys / sizeof kKeys[0];

static const std::vector<std::string>& table_keys() {
    static const std::vector<std::string> v(kKeys, kKeys + kKeyCount);
    return v;
}

// ---------------------------------------------------------------- report helpers

static nlohmann::json parse_report(const std::vector<op::NamedProject>& goldens) {
    return nlohmann::json::parse(op::feature_coverage_json(goldens));
}

static bool has_exact_keys(const nlohmann::json& o, std::initializer_list<const char*> want) {
    if (!o.is_object() || o.size() != want.size()) return false;
    for (const char* k : want)
        if (!o.contains(k)) return false;
    return true;
}

static std::vector<std::string> keys_of_features(const nlohmann::json& root) {
    std::vector<std::string> out;
    for (const auto& e : root.at("features")) out.push_back(e.at("key").get<std::string>());
    return out;
}

static std::vector<std::string> uncovered_of(const nlohmann::json& root) {
    return root.at("uncovered").get<std::vector<std::string>>();
}

static std::vector<std::string> covered_by(const nlohmann::json& root, const std::string& key) {
    for (const auto& e : root.at("features"))
        if (e.at("key").get<std::string>() == key) return e.at("covered_by").get<std::vector<std::string>>();
    return {};
}

static std::vector<std::string> golden_features_of(const nlohmann::json& root, const std::string& name) {
    for (const auto& g : root.at("goldens"))
        if (g.at("name").get<std::string>() == name) return g.at("features").get<std::vector<std::string>>();
    return {};
}

static bool marks(const nlohmann::json& root, const std::string& golden, const std::string& key) {
    const std::vector<std::string> f = golden_features_of(root, golden);
    for (const std::string& k : f)
        if (k == key) return true;
    return false;
}

// v with every element of drop removed (order preserved) — for "uncovered shrank by exactly these".
static std::vector<std::string> without(std::vector<std::string> v, const std::vector<std::string>& drop) {
    std::vector<std::string> out;
    for (const std::string& s : v) {
        bool skip = false;
        for (const std::string& d : drop)
            if (s == d) { skip = true; break; }
        if (!skip) out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------- project builders

// 120 bpm, 4/4, 48 kHz, zero tracks and zero patterns: marks NO feature.
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

static op::NamedProject named(const std::string& name, const op::Project& p) {
    op::NamedProject np;
    np.name = name;
    np.project = p;
    return np;
}

static op::Track sine_track(const std::string& name) {
    op::Track t;
    t.name = name;
    t.voice.kind = op::VoiceKind::Sine;
    return t;
}

static op::Project sampler_project() {  // one sampler track (no trim, no root note) + one plain note
    op::Project p = base_project();
    op::Track t;
    t.name = "S";
    t.voice.kind = op::VoiceKind::Sampler;
    t.voice.path = "kits/k.wav";
    p.tracks.push_back(t);
    op::Pattern pat;
    pat.track = 0;
    pat.notes.push_back(op::Note{});
    p.patterns.push_back(pat);
    return p;
}

static op::Project arranged_project() {  // sine track + one-note pattern + one plain clip
    op::Project p = base_project();
    p.tracks.push_back(sine_track("A"));
    op::Pattern pat;
    pat.track = 0;
    pat.notes.push_back(op::Note{});
    p.patterns.push_back(pat);
    op::Clip c;
    c.pattern = 0;
    c.start_bar = 0.0;
    p.arrangement.push_back(c);
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

    // --- empty input: the four top-level keys, version 1, the exact 32-row table in table order,
    //     every row with exactly three keys and a non-empty label, nothing covered, all uncovered.
    const std::string text = op::feature_coverage_json({});
    const nlohmann::json empty_root = nlohmann::json::parse(text);
    CHECK(has_exact_keys(empty_root, {"features", "goldens", "uncovered", "version"}),
          "fc.top_level_exactly_four_keys");
    CHECK(empty_root.at("version").is_number_integer() && empty_root.at("version").get<int>() == 1,
          "fc.version_is_json_integer_1");
    CHECK(empty_root.at("features").is_array() && empty_root.at("features").size() == kKeyCount,
          "fc.features_array_has_32_rows");
    CHECK(keys_of_features(empty_root) == table_keys(), "fc.feature_keys_exact_32_in_table_order");
    bool rows_ok = true;
    for (const auto& e : empty_root.at("features")) {
        rows_ok = rows_ok && has_exact_keys(e, {"covered_by", "key", "label"});
        rows_ok = rows_ok && e.at("label").is_string() && !e.at("label").get<std::string>().empty();
        rows_ok = rows_ok && e.at("covered_by").is_array() && e.at("covered_by").empty();
    }
    CHECK(rows_ok, "fc.every_row_has_three_keys_nonempty_label_and_empty_covered_by");
    CHECK(empty_root.at("features").at(13).at("key").get<std::string>() == "mute" &&
              empty_root.at("features").at(13).at("label").get<std::string>() == "track mute",
          "fc.label_of_mute_row_exact");
    CHECK(empty_root.at("features").at(20).at("key").get<std::string>() == "sampler_repitch" &&
              empty_root.at("features").at(20).at("label").get<std::string>() == "sampler root_note (repitch)",
          "fc.label_of_sampler_repitch_row_exact");
    CHECK(empty_root.at("goldens").is_array() && empty_root.at("goldens").empty(),
          "fc.empty_input_goldens_is_empty_array");
    CHECK(uncovered_of(empty_root) == table_keys(), "fc.empty_input_uncovered_is_every_table_key");
    CHECK(text.rfind("{\n  \"features\": [\n", 0) == 0, "fc.text_starts_with_dump2_features_header");
    CHECK(text.size() >= 2 && text[text.size() - 1] == '\n' && text[text.size() - 2] == '}' &&
              text[text.size() - 3] == '\n',
          "fc.text_ends_with_close_brace_and_exactly_one_trailing_newline");

    // --- a bare in-memory project marks NO feature at all.
    {
        const nlohmann::json r = parse_report({named("bare", base_project())});
        CHECK(golden_features_of(r, "bare").empty(), "fc.bare_project_marks_no_feature");
    }

    // --- mute vs solo stay separate rows.
    {
        op::Project m = base_project();
        m.tracks.push_back(sine_track("A"));
        m.tracks[0].mute = true;
        op::Project s = base_project();
        s.tracks.push_back(sine_track("A"));
        s.tracks[0].solo = true;
        const nlohmann::json r = parse_report({named("muteG", m), named("soloG", s)});
        CHECK(marks(r, "muteG", "mute") && !marks(r, "muteG", "solo"), "fc.mute_row_only_for_muted_track");
        CHECK(marks(r, "soloG", "solo") && !marks(r, "soloG", "mute"), "fc.solo_row_only_for_soloed_track");
    }

    // --- clip_fade_in vs clip_fade_out stay separate rows.
    {
        op::Project fi = arranged_project();
        fi.arrangement[0].fade_in_beats = 2.0;
        op::Project fo = arranged_project();
        fo.arrangement[0].fade_out_beats = 1.5;
        const nlohmann::json r = parse_report({named("fiG", fi), named("foG", fo)});
        CHECK(marks(r, "fiG", "clip_fade_in") && !marks(r, "fiG", "clip_fade_out"),
              "fc.fade_in_row_only_for_fade_in_clip");
        CHECK(marks(r, "foG", "clip_fade_out") && !marks(r, "foG", "clip_fade_in"),
              "fc.fade_out_row_only_for_fade_out_clip");
        CHECK(marks(r, "fiG", "arrangement") && marks(r, "foG", "arrangement"),
              "fc.arrangement_row_for_nonempty_arrangement");
    }

    // --- clip_offset_beats vs clip_length_beats stay separate rows.
    {
        op::Project off = arranged_project();
        off.arrangement[0].offset_beats = 0.5;
        op::Project len = arranged_project();
        len.arrangement[0].length_beats = 2.0;
        const nlohmann::json r = parse_report({named("offG", off), named("lenG", len)});
        CHECK(marks(r, "offG", "clip_offset_beats") && !marks(r, "offG", "clip_length_beats"),
              "fc.offset_row_only_for_nonzero_offset");
        CHECK(marks(r, "lenG", "clip_length_beats") && !marks(r, "lenG", "clip_offset_beats"),
              "fc.length_row_only_for_present_length");
    }

    // --- sampler_slice (has_end_s) vs sampler_repitch (root_note) stay separate rows.
    {
        op::Project sl = sampler_project();
        sl.tracks[0].voice.has_end_s = true;
        sl.tracks[0].voice.end_s = 0.5;
        op::Project rp = sampler_project();
        rp.tracks[0].voice.root_note = 60;
        const nlohmann::json r = parse_report({named("sliceG", sl), named("repitchG", rp)});
        CHECK(marks(r, "sliceG", "sampler_slice") && !marks(r, "sliceG", "sampler_repitch"),
              "fc.slice_row_only_for_has_end_s");
        CHECK(marks(r, "repitchG", "sampler_repitch") && !marks(r, "repitchG", "sampler_slice"),
              "fc.repitch_row_only_for_root_note");
    }

    // --- note_pitch (midi_note >= 0) vs note_velocity (!= 1.0) stay separate rows.
    {
        op::Project pitch = base_project();
        pitch.tracks.push_back(sine_track("A"));
        op::Pattern pp;
        pp.track = 0;
        op::Note n1;
        n1.midi_note = 60;  // velocity stays 1.0
        pp.notes.push_back(n1);
        pitch.patterns.push_back(pp);
        op::Project vel = base_project();
        vel.tracks.push_back(sine_track("A"));
        op::Pattern vp;
        vp.track = 0;
        op::Note n2;
        n2.midi_note = -1;  // absent
        n2.velocity = 0.5;
        vp.notes.push_back(n2);
        vel.patterns.push_back(vp);
        const nlohmann::json r = parse_report({named("pitchG", pitch), named("velG", vel)});
        CHECK(marks(r, "pitchG", "note_pitch") && !marks(r, "pitchG", "note_velocity"),
              "fc.pitch_row_only_for_midi_note");
        CHECK(marks(r, "velG", "note_velocity") && !marks(r, "velG", "note_pitch"),
              "fc.velocity_row_only_for_nonunit_velocity");
    }

    // --- beats_per_bar vs time_signature rows: the loaded shape of 3/4 (beats_per_bar 3, NO
    //     time_signature value) marks only beats_per_bar_not_4; 6/8 (both present) marks both.
    {
        op::Project p34 = base_project();
        p34.beats_per_bar = 3;
        op::Project p68 = base_project();
        p68.beats_per_bar = 3;
        p68.time_signature = op::TimeSignature{6, 8};
        const nlohmann::json r = parse_report({named("p34G", p34), named("p68G", p68)});
        CHECK(marks(r, "p34G", "beats_per_bar_not_4") && !marks(r, "p34G", "time_signature"),
              "fc.loaded_3_4_shape_marks_bbp_only_not_time_signature");
        CHECK(marks(r, "p68G", "beats_per_bar_not_4") && marks(r, "p68G", "time_signature"),
              "fc.six_eight_marks_both_rows");
    }
    // --- the loader itself: a file with "time_signature": {num 3, den 4} loads as beats_per_bar 3
    //     and NO time_signature value, so the report marks beats_per_bar_not_4 only (spec example).
    {
        const fs::path opj = scratch_dir() / "ts34.opj";
        {
            std::ofstream f(opj, std::ios::binary);
            f << "{\"format_version\":0,\"bpm\":120,\"sample_rate\":48000,\"seed\":1,\"length_bars\":1,"
                 "\"beats_per_bar\":3,\"tracks\":[{\"name\":\"T\",\"voice\":{\"kind\":\"sine\",\"freq_hz\":440}}],"
                 "\"patterns\":[{\"track\":0,\"notes\":[{\"t\":0,\"dur\":1}]}],"
                 "\"time_signature\":{\"num\":3,\"den\":4}}\n";
        }
        op::Project loaded;
        std::string err;
        CHECK(op::load_project(opj.string(), loaded, err) && loaded.beats_per_bar == 3 &&
                  !loaded.time_signature.has_value(),
              "fc.loader_normalizes_3_4_to_bbp_3_no_time_signature_value");
        const nlohmann::json r = parse_report({named("ts34", loaded)});
        CHECK(marks(r, "ts34", "beats_per_bar_not_4") && !marks(r, "ts34", "time_signature"),
              "fc.loaded_3_4_file_marks_bbp_not_time_signature");
    }

    // --- synth_v1: a legacy synth (defaults) marks voice_kind_synth but NOT synth_v1; each of the
    //     three v1 markers (non-legacy waveform, ADSR, cutoff > 0) marks it.
    {
        auto synth_base = [] {
            op::Project p = base_project();
            op::Track t;
            t.name = "Y";
            t.voice.kind = op::VoiceKind::Synth;
            p.tracks.push_back(t);
            return p;
        };
        const op::Project legacy = synth_base();
        op::Project wf = synth_base();
        wf.tracks[0].voice.waveform = op::Waveform::Sine;
        op::Project adsr = synth_base();
        adsr.tracks[0].voice.has_adsr = true;
        op::Project cut = synth_base();
        cut.tracks[0].voice.cutoff_hz = 2500.0;
        const nlohmann::json r = parse_report(
            {named("legacyG", legacy), named("wfG", wf), named("adsrG", adsr), named("cutG", cut)});
        CHECK(marks(r, "legacyG", "voice_kind_synth") && !marks(r, "legacyG", "synth_v1"),
              "fc.legacy_synth_not_synth_v1");
        CHECK(marks(r, "wfG", "synth_v1"), "fc.nonlegacy_waveform_marks_synth_v1");
        CHECK(marks(r, "adsrG", "synth_v1"), "fc.adsr_marks_synth_v1");
        CHECK(marks(r, "cutG", "synth_v1"), "fc.cutoff_marks_synth_v1");
    }

    // --- project-level rows: tempo_map, loop_region, attachments, master_gain_db.
    {
        op::Project tm = base_project();
        tm.tempo_map.push_back(op::TempoPoint{0.0, 95.0});
        op::Project lp = base_project();
        lp.loop = op::LoopRegion{1.0, 3.0};
        op::Project at = base_project();
        at.attachments.push_back(op::Attachment{"stems/a.wav", "stem", "test"});
        op::Project mg = base_project();
        mg.master_gain_db = 1.5f;
        const nlohmann::json r = parse_report(
            {named("tmG", tm), named("lpG", lp), named("atG", at), named("mgG", mg)});
        CHECK(marks(r, "tmG", "tempo_map"), "fc.tempo_map_row_for_nonempty_map");
        CHECK(marks(r, "lpG", "loop_region"), "fc.loop_region_row_for_present_loop");
        CHECK(marks(r, "atG", "attachments"), "fc.attachments_row_for_nonempty_list");
        CHECK(marks(r, "mgG", "master_gain_db"), "fc.master_gain_row_for_nonzero_fader");
    }

    // --- pattern rows: pattern_id, pattern_length_beats, swing; plus the choke_group row.
    {
        op::Project p = base_project();
        p.tracks.push_back(sine_track("A"));
        op::Track s;
        s.name = "S";
        s.voice.kind = op::VoiceKind::Sampler;
        s.voice.path = "kits/k.wav";
        s.choke_group = 2;
        p.tracks.push_back(s);
        op::Pattern pat;
        pat.track = 0;
        pat.id = "P1";
        pat.swing = 0.2;
        pat.length_beats = 4.0;
        pat.notes.push_back(op::Note{});
        p.patterns.push_back(pat);
        const nlohmann::json r = parse_report({named("patG", p)});
        CHECK(marks(r, "patG", "pattern_id"), "fc.pattern_id_row_for_named_pattern");
        CHECK(marks(r, "patG", "pattern_length_beats"), "fc.pattern_length_row_for_present_length");
        CHECK(marks(r, "patG", "swing"), "fc.swing_row_for_nonzero_swing");
        CHECK(marks(r, "patG", "choke_group"), "fc.choke_row_for_nonzero_group");
        CHECK(marks(r, "patG", "voice_kind_sampler"), "fc.sampler_kind_row_for_sampler_voice");
    }

    // --- project/track scalars: sample_rate_not_48k, track_gain_db, pan.
    {
        op::Project sr = base_project();
        sr.sample_rate = 44100;
        op::Project tg = base_project();
        op::Track t1 = sine_track("A");
        t1.gain_db = -3.0f;
        tg.tracks.push_back(t1);
        op::Project pn = base_project();
        op::Track t2 = sine_track("A");
        t2.pan = 0.5f;
        pn.tracks.push_back(t2);
        const nlohmann::json r = parse_report({named("srG", sr), named("tgG", tg), named("pnG", pn)});
        CHECK(marks(r, "srG", "sample_rate_not_48k"), "fc.sample_rate_row_for_44100");
        CHECK(marks(r, "tgG", "track_gain_db"), "fc.track_gain_row_for_nonzero_gain");
        CHECK(marks(r, "pnG", "pan"), "fc.pan_row_for_nonzero_pan");
    }

    // --- metronome_accent, clip_repeats, clip_gain_db; all four voice_kind rows together.
    {
        op::Project acc = base_project();
        op::Track m;
        m.name = "M";
        m.voice.kind = op::VoiceKind::Metronome;
        m.voice.accent_every = 3;
        acc.tracks.push_back(m);
        op::Project rep = arranged_project();
        rep.arrangement[0].repeats = 2;
        op::Project cg = arranged_project();
        cg.arrangement[0].gain_db = 2.0;
        op::Project kinds = base_project();
        op::Track mk;
        mk.name = "M";
        mk.voice.kind = op::VoiceKind::Metronome;
        op::Track sk = sine_track("S");
        op::Track pk;
        pk.name = "P";
        pk.voice.kind = op::VoiceKind::Sampler;
        pk.voice.path = "kits/k.wav";
        op::Track yk;
        yk.name = "Y";
        yk.voice.kind = op::VoiceKind::Synth;
        kinds.tracks = {mk, sk, pk, yk};
        const nlohmann::json r =
            parse_report({named("accG", acc), named("repG", rep), named("cgG", cg), named("kindsG", kinds)});
        CHECK(marks(r, "accG", "metronome_accent"), "fc.accent_row_for_accent_every_3");
        CHECK(marks(r, "repG", "clip_repeats"), "fc.repeats_row_for_repeats_2");
        CHECK(marks(r, "cgG", "clip_gain_db"), "fc.clip_gain_row_for_nonzero_gain");
        CHECK(marks(r, "kindsG", "voice_kind_metronome") && marks(r, "kindsG", "voice_kind_sampler") &&
                  marks(r, "kindsG", "voice_kind_sine") && marks(r, "kindsG", "voice_kind_synth"),
              "fc.all_four_voice_kind_rows_together");
    }

    // --- goldens AND covered_by are name-ascending whatever the input order.
    {
        auto muted_sine = [] {
            op::Project p = base_project();
            p.tracks.push_back(sine_track("A"));
            p.tracks[0].mute = true;
            return p;
        };
        const nlohmann::json r = parse_report({named("zeta", muted_sine()),
                                               named("alpha", muted_sine()),
                                               named("mid", muted_sine())});
        std::vector<std::string> golden_names;
        for (const auto& g : r.at("goldens")) golden_names.push_back(g.at("name").get<std::string>());
        const std::vector<std::string> want_order = {"alpha", "mid", "zeta"};
        CHECK(golden_names == want_order, "fc.goldens_sorted_by_name_regardless_of_input_order");
        const std::vector<std::string> covered_mute = covered_by(r, "mute");
        CHECK(covered_mute == want_order, "fc.covered_by_sorted_by_name");
        bool golden_keys_ok = true;
        for (const auto& g : r.at("goldens"))
            golden_keys_ok = golden_keys_ok && has_exact_keys(g, {"features", "name"});
        CHECK(golden_keys_ok, "fc.golden_entry_exactly_two_keys");
    }

    // --- uncovered shrinks by exactly the newly covered keys.
    {
        op::Project m = base_project();
        m.tracks.push_back(sine_track("A"));
        m.tracks[0].mute = true;
        op::Project s = base_project();
        s.tracks.push_back(sine_track("A"));
        s.tracks[0].solo = true;
        const nlohmann::json r1 = parse_report({named("mG", m)});
        const nlohmann::json r2 = parse_report({named("mG", m), named("sG", s)});
        // Both projects carry a sine track, so voice_kind_sine shrinks too.
        CHECK(uncovered_of(r1) == without(table_keys(), {"mute", "voice_kind_sine"}),
              "fc.uncovered_shrank_by_mute_only");
        CHECK(uncovered_of(r2) == without(table_keys(), {"mute", "solo", "voice_kind_sine"}),
              "fc.uncovered_shrank_by_mute_and_solo");
    }

    // --- two calls with equal input are byte-identical (pure function).
    {
        std::vector<op::NamedProject> in;
        in.push_back(named("a", arranged_project()));
        in.push_back(named("b", sampler_project()));
        CHECK(op::feature_coverage_json(in) == op::feature_coverage_json(in),
              "fc.two_calls_byte_identical");
    }

    // --- equal names: one entry per input golden, no de-duplication, covered_by lists the name twice.
    {
        std::vector<op::NamedProject> in;
        op::Project m = base_project();
        m.tracks.push_back(sine_track("A"));
        m.tracks[0].mute = true;
        in.push_back(named("dup", m));
        in.push_back(named("dup", m));  // the SAME golden twice: contributes twice, no de-duplication
        const nlohmann::json r = nlohmann::json::parse(op::feature_coverage_json(in));
        CHECK(r.at("goldens").size() == 2 &&
                  r.at("goldens").at(0).at("name").get<std::string>() == "dup" &&
                  r.at("goldens").at(1).at("name").get<std::string>() == "dup",
              "fc.equal_names_keep_one_entry_per_input_golden");
        const std::vector<std::string> want_dup = {"dup", "dup"};
        CHECK(covered_by(r, "mute") == want_dup, "fc.covered_by_repeats_equal_names");
    }

    // ---------------------------------------------------------------- CLI contract

    // A plain sine project saved to a file: exit 0 despite uncovered features; stdout is exactly
    // feature_coverage_json on the loaded project; name is the file stem; text starts with the dump(2) header.
    {
        const fs::path opj = scratch_dir() / "simple.opj";
        std::string err;
        op::Project p = base_project();
        p.tracks.push_back(sine_track("A"));
        op::Pattern pat;
        pat.track = 0;
        pat.notes.push_back(op::Note{});
        p.patterns.push_back(pat);
        CHECK(op::save_project(opj.string(), p, err), "fc.cli_saved_simple_project");
        const std::string bytes_before = read_file(opj);

        const Run r1 = run_opd("golden-coverage \"" + opj.string() + "\"");
        CHECK(r1.code == 0 && !r1.out.empty(), "fc.cli_exit_0_even_with_uncovered_features");
        CHECK(r1.out.rfind("{\n  \"features\": [\n", 0) == 0, "fc.cli_stdout_starts_with_dump2_header");
        op::Project loaded;
        CHECK(op::load_project(opj.string(), loaded, err), "fc.cli_reload_simple_project");
        CHECK(r1.out == op::feature_coverage_json({named("simple", loaded)}),
              "fc.cli_stdout_equals_engine_function");
        nlohmann::json root = r1.out.empty() ? nlohmann::json::object() : nlohmann::json::parse(r1.out);
        CHECK(r1.code == 0 && root.at("goldens").size() == 1 &&
                  root.at("goldens").at(0).at("name").get<std::string>() == "simple" &&
                  golden_features_of(root, "simple") == std::vector<std::string>{"voice_kind_sine"},
              "fc.cli_name_is_file_stem_plain_project_marks_only_voice_kind_sine");

        const Run r2 = run_opd("golden-coverage \"" + opj.string() + "\"");
        CHECK(r1.out == r2.out, "fc.cli_two_runs_byte_identical");
        CHECK(read_file(opj) == bytes_before, "fc.cli_project_file_bytes_unchanged");

        // A repeated file argument: two entries with the same name (no de-duplication).
        const Run r3 = run_opd("golden-coverage \"" + opj.string() + "\" \"" + opj.string() + "\"");
        nlohmann::json root3 = r3.out.empty() ? nlohmann::json::object() : nlohmann::json::parse(r3.out);
        CHECK(r3.code == 0 && root3.contains("goldens") && root3.at("goldens").size() == 2,
              "fc.cli_repeated_argument_contributes_two_same_name_entries");
    }

    // A directory: only regular *.opj files (suffix case-sensitive), sorted by file name ascending.
    {
        const fs::path sub = scratch_dir() / "dir";
        fs::create_directories(sub, ec);
        std::string err;
        bool saved_ok = true;
        for (const char* n : {"zulu", "alpha"}) {
            op::Project p = base_project();
            p.tracks.push_back(sine_track(n));
            saved_ok = saved_ok && op::save_project((sub / (std::string(n) + ".opj")).string(), p, err);
        }
        CHECK(saved_ok, "fc.cli_dir_projects_saved");
        {  // distractors: wrong suffix, uppercase suffix, non-file entries
            std::ofstream(sub / "notme.opx", std::ios::binary) << "x";
            std::ofstream(sub / "UPPER.OPJ", std::ios::binary) << "x";
            std::ofstream(sub / "readme.txt", std::ios::binary) << "x";
        }
        const Run r = run_opd("golden-coverage \"" + sub.string() + "\"");
        nlohmann::json root = r.out.empty() ? nlohmann::json::object() : nlohmann::json::parse(r.out);
        std::vector<std::string> names;
        for (const auto& g : root.at("goldens")) names.push_back(g.at("name").get<std::string>());
        const std::vector<std::string> want_names = {"alpha", "zulu"};
        CHECK(r.code == 0 && names == want_names,
              "fc.cli_directory_scans_opj_only_sorted_by_file_name");
        std::size_t entries = 0;
        for (fs::directory_iterator it(sub, ec); !ec && it != fs::directory_iterator(); ++it) ++entries;
        CHECK(entries == 5, "fc.cli_directory_run_created_no_files");  // 2 .opj + .opx + .OPJ + .txt, unchanged
    }

    // Errors: nothing on stdout. No arguments → exit 2 with the exact message.
    {
        const Run r = run_opd("golden-coverage");
        CHECK(r.code == 2 && r.out.empty() &&
                  r.err.find("golden-coverage needs at least one <dir or .opj>") != std::string::npos,
              "fc.cli_no_arguments_exit_2");
    }
    // An option token → exit 2 usage.
    {
        const Run r = run_opd("golden-coverage --json \"" + (scratch_dir() / "simple.opj").string() + "\"");
        CHECK(r.code == 2 && r.out.empty() &&
                  r.err.find("unknown arg '--json'") != std::string::npos,
              "fc.cli_unknown_option_exit_2");
    }
    // A directory without any .opj → exit 2.
    {
        const fs::path empty = scratch_dir() / "emptydir";
        fs::create_directories(empty, ec);
        const Run r = run_opd("golden-coverage \"" + empty.string() + "\"");
        CHECK(r.code == 2 && r.out.empty() &&
                  r.err.find("no .opj files in " + empty.string()) != std::string::npos,
              "fc.cli_directory_without_opj_exit_2");
    }
    // A path that does not exist → exit 3, detail = the path; resolved BEFORE anything is loaded.
    {
        const std::string missing = (scratch_dir() / "nope.opj").string();
        const Run r = run_opd("golden-coverage \"" + missing + "\"");
        CHECK(r.code == 3 && r.out.empty() &&
                  r.err.find("not_found: " + missing) != std::string::npos,
              "fc.cli_missing_path_exit_3_detail_is_path");
        // A valid project first, a missing path second: still exit 3 with empty stdout.
        const Run r2 = run_opd("golden-coverage \"" + (scratch_dir() / "simple.opj").string() +
                               "\" \"" + missing + "\"");
        CHECK(r2.code == 3 && r2.out.empty(), "fc.cli_all_args_resolved_before_any_load");
    }
    // An unparseable project → exit 4 with the loader's own message, empty stdout.
    {
        const fs::path bad = scratch_dir() / "garbage.opj";
        {
            std::ofstream f(bad, std::ios::binary);
            f << "{ this is not json at all\n";
        }
        const Run r = run_opd("golden-coverage \"" + bad.string() + "\"");
        CHECK(r.code == 4 && r.out.empty() && r.err.find("error: parse: ") != std::string::npos,
              "fc.cli_unparseable_project_exit_4_empty_stdout");
    }

    fs::remove_all(scratch_dir(), ec);
    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures);
    return g_failures;
}
