// engine/feature_coverage.cpp — WP-0211: the feature table and report builder behind
// `opd golden-coverage` (contract in engine/feature_coverage.hpp, docs/spec/golden-coverage.md).
// The 32 rows below ARE the feature set: keys ASCII-ascending, and that is the table order used
// everywhere in the report. Nothing else is a feature; render-only options (asset sample-rate
// conversion, --limit-dbfs, normalize flags) are NOT model features.
#include "engine/feature_coverage.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace op {
namespace {

// --- one plain predicate per table row (docs/spec/golden-coverage.md §Feature table). Each reads
// the LOADED project, so loader normalizations apply. "Any X" = some item of the named kind
// satisfies the condition; project-level rows are checked on the project itself.

bool arrangement_nonempty(const Project& p) { return !p.arrangement.empty(); }

bool attachments_nonempty(const Project& p) { return !p.attachments.empty(); }

bool beats_per_bar_not_4(const Project& p) { return p.beats_per_bar != 4; }

bool any_choke_group(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.choke_group != 0) return true;
    return false;
}

bool any_clip_fade_in(const Project& p) {
    for (const Clip& c : p.arrangement)
        if (c.fade_in_beats > 0.0) return true;
    return false;
}

bool any_clip_fade_out(const Project& p) {
    for (const Clip& c : p.arrangement)
        if (c.fade_out_beats > 0.0) return true;
    return false;
}

bool any_clip_gain_db(const Project& p) {
    for (const Clip& c : p.arrangement)
        if (c.gain_db != 0.0) return true;
    return false;
}

bool any_clip_length_beats(const Project& p) {
    for (const Clip& c : p.arrangement)
        if (c.length_beats.has_value()) return true;
    return false;
}

bool any_clip_offset_beats(const Project& p) {
    for (const Clip& c : p.arrangement)
        if (c.offset_beats != 0.0) return true;
    return false;
}

bool any_clip_repeats(const Project& p) {
    for (const Clip& c : p.arrangement)
        if (c.repeats != 1) return true;
    return false;
}

bool loop_present(const Project& p) { return p.loop.has_value(); }

bool master_gain_nonzero(const Project& p) { return p.master_gain_db != 0.0f; }

bool any_accent_every(const Project& p) {  // accent_every is a field of every voice kind, not just metronome
    for (const Track& t : p.tracks)
        if (t.voice.accent_every != 4) return true;
    return false;
}

bool any_mute(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.mute) return true;
    return false;
}

bool any_midi_note(const Project& p) {
    for (const Pattern& pat : p.patterns)
        for (const Note& n : pat.notes)
            if (n.midi_note >= 0) return true;
    return false;
}

bool any_note_velocity(const Project& p) {
    for (const Pattern& pat : p.patterns)
        for (const Note& n : pat.notes)
            if (n.velocity != 1.0) return true;
    return false;
}

bool any_pan(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.pan != 0.0f) return true;
    return false;
}

bool any_pattern_id(const Project& p) {
    for (const Pattern& pat : p.patterns)
        if (!pat.id.empty()) return true;
    return false;
}

bool any_pattern_length_beats(const Project& p) {
    for (const Pattern& pat : p.patterns)
        if (pat.length_beats.has_value()) return true;
    return false;
}

bool sample_rate_not_48k(const Project& p) { return p.sample_rate != 48000; }

bool any_root_note(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.voice.root_note >= 0) return true;
    return false;
}

bool any_has_end_s(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.voice.has_end_s) return true;
    return false;
}

bool any_solo(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.solo) return true;
    return false;
}

bool any_swing(const Project& p) {
    for (const Pattern& pat : p.patterns)
        if (pat.swing != 0.0) return true;
    return false;
}

bool any_synth_v1(const Project& p) {  // any v1 marker: non-legacy waveform, ADSR envelope or a filter cutoff
    for (const Track& t : p.tracks) {
        const Voice& v = t.voice;
        if (v.waveform != Waveform::Legacy || v.has_adsr || v.cutoff_hz > 0.0) return true;
    }
    return false;
}

bool tempo_map_nonempty(const Project& p) { return !p.tempo_map.empty(); }

bool time_signature_present(const Project& p) { return p.time_signature.has_value(); }

bool any_track_gain_db(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.gain_db != 0.0f) return true;
    return false;
}

bool any_voice_metronome(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.voice.kind == VoiceKind::Metronome) return true;
    return false;
}

bool any_voice_sampler(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.voice.kind == VoiceKind::Sampler) return true;
    return false;
}

bool any_voice_sine(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.voice.kind == VoiceKind::Sine) return true;
    return false;
}

bool any_voice_synth(const Project& p) {
    for (const Track& t : p.tracks)
        if (t.voice.kind == VoiceKind::Synth) return true;
    return false;
}

struct FeatureRow {
    const char* key;
    const char* label;
    bool (*covers)(const Project&);
};

// The whole feature set — key, label and predicate exactly per docs/spec/golden-coverage.md.
// Keys are ASCII-ascending and that IS the report's table order. Do not add, rename or re-order.
const FeatureRow kFeatures[] = {
    {"arrangement", "explicit arrangement clips", arrangement_nonempty},
    {"attachments", "attachment references", attachments_nonempty},
    {"beats_per_bar_not_4", "beats_per_bar != 4", beats_per_bar_not_4},
    {"choke_group", "sampler choke group", any_choke_group},
    {"clip_fade_in", "clip fade-in", any_clip_fade_in},
    {"clip_fade_out", "clip fade-out", any_clip_fade_out},
    {"clip_gain_db", "clip gain dB", any_clip_gain_db},
    {"clip_length_beats", "clip trim length", any_clip_length_beats},
    {"clip_offset_beats", "clip trim offset", any_clip_offset_beats},
    {"clip_repeats", "clip repeats > 1", any_clip_repeats},
    {"loop_region", "transport loop region", loop_present},
    {"master_gain_db", "master fader", master_gain_nonzero},
    {"metronome_accent", "metronome accent_every != 4", any_accent_every},
    {"mute", "track mute", any_mute},
    {"note_pitch", "note MIDI pitch", any_midi_note},
    {"note_velocity", "note velocity != 1.0", any_note_velocity},
    {"pan", "track pan != 0", any_pan},
    {"pattern_id", "named pattern", any_pattern_id},
    {"pattern_length_beats", "explicit pattern length", any_pattern_length_beats},
    {"sample_rate_not_48k", "sample_rate != 48000", sample_rate_not_48k},
    {"sampler_repitch", "sampler root_note (repitch)", any_root_note},
    {"sampler_slice", "sampler slice end_s", any_has_end_s},
    {"solo", "track solo", any_solo},
    {"swing", "pattern swing != 0", any_swing},
    {"synth_v1", "synth voice v1", any_synth_v1},
    {"tempo_map", "tempo map", tempo_map_nonempty},
    {"time_signature", "time signature (den != 4)", time_signature_present},
    {"track_gain_db", "track fader", any_track_gain_db},
    {"voice_kind_metronome", "metronome voice", any_voice_metronome},
    {"voice_kind_sampler", "sampler voice", any_voice_sampler},
    {"voice_kind_sine", "sine voice", any_voice_sine},
    {"voice_kind_synth", "synth voice", any_voice_synth},
};

constexpr std::size_t kFeatureCount = sizeof(kFeatures) / sizeof(kFeatures[0]);

}  // namespace

std::string feature_coverage_json(const std::vector<NamedProject>& goldens) {
    // Golden entries are name-ascending regardless of input order; iterating the goldens in that
    // order also makes every covered_by list ascending (duplicate names contribute one entry each,
    // in input order — stable_sort — and their names appear that many times in covered_by).
    std::vector<std::size_t> order(goldens.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return goldens[a].name < goldens[b].name;
    });

    // Evaluate every predicate on every golden exactly once: hit[g][f] = golden g exercises table row f.
    std::vector<std::vector<char>> hit(goldens.size(), std::vector<char>(kFeatureCount, 0));
    for (std::size_t g = 0; g < goldens.size(); ++g)
        for (std::size_t f = 0; f < kFeatureCount; ++f)
            if (kFeatures[f].covers(goldens[g].project)) hit[g][f] = 1;

    nlohmann::json features = nlohmann::json::array();
    nlohmann::json uncovered = nlohmann::json::array();
    for (std::size_t f = 0; f < kFeatureCount; ++f) {
        nlohmann::json covered_by = nlohmann::json::array();
        for (std::size_t g : order)
            if (hit[g][f]) covered_by.push_back(goldens[g].name);
        if (covered_by.empty()) uncovered.push_back(kFeatures[f].key);
        nlohmann::json entry;
        entry["covered_by"] = std::move(covered_by);
        entry["key"] = kFeatures[f].key;
        entry["label"] = kFeatures[f].label;
        features.push_back(std::move(entry));
    }

    nlohmann::json golden_arr = nlohmann::json::array();
    for (std::size_t g : order) {
        nlohmann::json feats = nlohmann::json::array();
        for (std::size_t f = 0; f < kFeatureCount; ++f)
            if (hit[g][f]) feats.push_back(kFeatures[f].key);
        nlohmann::json entry;
        entry["features"] = std::move(feats);
        entry["name"] = goldens[g].name;
        golden_arr.push_back(std::move(entry));
    }

    nlohmann::json root;
    root["features"] = std::move(features);
    root["goldens"] = std::move(golden_arr);
    root["uncovered"] = std::move(uncovered);
    root["version"] = 1;
    return root.dump(2) + "\n";
}

}  // namespace op
