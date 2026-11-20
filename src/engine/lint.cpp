// engine/lint.cpp — read-only project sanity checks (docs/spec/lint.md, WP-0170).
// Boring by design: one loop per rule, in the spec's rule order, ascending index within each rule.
// Nothing here mutates the project; only rule 8 looks at the filesystem (exists(), error_code overload).
#include "engine/lint.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace op::lint {
namespace {

// JSON string escaping — the spec pins this exact mechanism (nlohmann is on the include path via opengine).
std::string esc(const std::string& s) { return nlohmann::json(s).dump(); }

}  // namespace

std::vector<Issue> lint_project(const Project& p, const std::string& base_dir) {
    std::vector<Issue> out;
    const double bpb = static_cast<double>(p.beats_per_bar);
    const double len_beats = p.length_bars * bpb;

    // Rule 1 (warning track_no_notes): no pattern with track == i has a note.
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        bool has_note = false;
        for (const Pattern& pat : p.patterns) {
            if (pat.track == static_cast<int>(i) && !pat.notes.empty()) { has_note = true; break; }
        }
        if (!has_note)
            out.push_back({"warning", "track_no_notes", "tracks[" + std::to_string(i) + "]",
                           "track has no notes"});
    }

    // Rule 2 (warning pattern_empty): pattern has zero notes.
    for (std::size_t j = 0; j < p.patterns.size(); ++j) {
        if (p.patterns[j].notes.empty())
            out.push_back({"warning", "pattern_empty", "patterns[" + std::to_string(j) + "]",
                           "pattern has no notes"});
    }

    // Rule 3 (warning pattern_unused): arrangement non-empty and no clip references the pattern.
    // Empty arrangement = legacy semantics (every pattern once at bar 0), so nothing is "unused".
    if (!p.arrangement.empty()) {
        for (std::size_t j = 0; j < p.patterns.size(); ++j) {
            bool referenced = false;
            for (const Clip& c : p.arrangement) {
                if (c.pattern == static_cast<int>(j)) { referenced = true; break; }
            }
            if (!referenced)
                out.push_back({"warning", "pattern_unused", "patterns[" + std::to_string(j) + "]",
                               "pattern is not referenced by any clip"});
        }
    }

    // Rule 4 (warning clip_duplicate): clip k repeats an earlier clip m — same pattern, exactly equal
    // start_bar. m = the FIRST such clip; one issue per clip k.
    for (std::size_t k = 0; k < p.arrangement.size(); ++k) {
        for (std::size_t m = 0; m < k; ++m) {
            if (p.arrangement[m].pattern == p.arrangement[k].pattern &&
                p.arrangement[m].start_bar == p.arrangement[k].start_bar) {
                out.push_back({"warning", "clip_duplicate", "arrangement[" + std::to_string(k) + "]",
                               "duplicate of arrangement[" + std::to_string(m) + "]"});
                break;
            }
        }
    }

    // Rule 5 (warning clip_past_end), then rule 6 (warning clip_overruns_length — defined as NOT rule 5).
    for (std::size_t k = 0; k < p.arrangement.size(); ++k) {
        const Clip& c = p.arrangement[k];
        const std::string path = "arrangement[" + std::to_string(k) + "]";
        if (c.start_bar >= p.length_bars) {
            out.push_back({"warning", "clip_past_end", path, "clip starts at or after length_bars"});
            continue;  // rule 6 only applies to clips that are not past the end
        }
        double extent = 0.0;
        if (c.pattern >= 0 && static_cast<std::size_t>(c.pattern) < p.patterns.size())
            extent = pattern_extent(p.patterns[static_cast<std::size_t>(c.pattern)]);  // honours length_beats (WP-0162)
        if (extent > 0.0 && c.start_bar * bpb + c.repeats * extent > len_beats + 1e-9)
            out.push_back({"warning", "clip_overruns_length", path, "clip ends after length_bars"});
    }

    // Rule 7 (warning notes_past_end): only with an EMPTY arrangement (legacy playback): notes at or past
    // the end of the song are never heard. N = count per pattern.
    if (p.arrangement.empty()) {
        for (std::size_t j = 0; j < p.patterns.size(); ++j) {
            std::size_t n = 0;
            for (const Note& note : p.patterns[j].notes)
                if (note.t_beats >= len_beats) ++n;
            if (n >= 1)
                out.push_back({"warning", "notes_past_end", "patterns[" + std::to_string(j) + "]",
                               std::to_string(n) + " note(s) start at or after length_bars"});
        }
    }

    // Rule 8 (error sampler_file_missing): rule 8 file resolution per spec — relative paths resolve against
    // base_dir; exists() with error_code, never throws. The message quotes voice.path as stored.
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        const Voice& v = p.tracks[i].voice;
        if (v.kind != VoiceKind::Sampler) continue;
        std::string resolved = v.path;
        if (!std::filesystem::path(resolved).is_absolute() && !base_dir.empty())
            resolved = (std::filesystem::path(base_dir) / std::filesystem::path(v.path)).string();
        std::error_code ec;
        if (!std::filesystem::exists(resolved, ec))
            out.push_back({"error", "sampler_file_missing", "tracks[" + std::to_string(i) + "].voice.path",
                           "sample file not found: " + v.path});
    }

    // Rule 9 (info track_muted): ascending index.
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        if (p.tracks[i].mute)
            out.push_back({"info", "track_muted", "tracks[" + std::to_string(i) + "]", "track is muted"});
    }

    // Rule 10 (info track_silenced_by_solo): some track solos, and this one neither solos nor mutes
    // (mute already has its own issue — a muted track is not additionally reported here).
    bool any_solo = false;
    for (const Track& t : p.tracks) {
        if (t.solo) { any_solo = true; break; }
    }
    if (any_solo) {
        for (std::size_t i = 0; i < p.tracks.size(); ++i) {
            const Track& t = p.tracks[i];
            if (!t.solo && !t.mute)
                out.push_back({"info", "track_silenced_by_solo", "tracks[" + std::to_string(i) + "]",
                               "silenced because another track is soloed"});
        }
    }

    // Rule 11 (warning tempo_point_past_end): a tempo change the song never reaches.
    for (std::size_t k = 0; k < p.tempo_map.size(); ++k) {
        if (p.tempo_map[k].bar >= p.length_bars)
            out.push_back({"warning", "tempo_point_past_end", "tempo_map[" + std::to_string(k) + "]",
                           "tempo change at or after length_bars"});
    }

    return out;
}

std::string to_json(const std::vector<Issue>& issues) {
    int errors = 0, warnings = 0, infos = 0;
    for (const Issue& is : issues) {
        if (is.severity == "error") ++errors;
        else if (is.severity == "warning") ++warnings;
        else ++infos;
    }
    std::string out = "{\"version\":1,\"errors\":" + std::to_string(errors) +
                      ",\"warnings\":" + std::to_string(warnings) +
                      ",\"infos\":" + std::to_string(infos) + ",\"issues\":[";
    for (std::size_t i = 0; i < issues.size(); ++i) {
        if (i > 0) out += ",";
        const Issue& is = issues[i];
        out += "{\"severity\":" + esc(is.severity) + ",\"code\":" + esc(is.code) +
               ",\"path\":" + esc(is.path) + ",\"message\":" + esc(is.message) + "}";
    }
    out += "]}\n";
    return out;
}

}  // namespace op::lint
