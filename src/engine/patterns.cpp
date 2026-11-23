// engine/patterns.cpp — pattern-bar listing as canonical JSON (WP-0228). Pure view over the model:
// no file I/O, no clock, no randomness, no project mutation. The effective loop length is taken from
// op::pattern_extent (model.cpp) — the renderer's own source of truth — never re-derived here.

#include "engine/patterns.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace op {

std::string patterns_json(const Project& p) {
    nlohmann::json arr = nlohmann::json::array();
    for (std::size_t i = 0; i < p.patterns.size(); ++i) {
        const Pattern& pat = p.patterns[i];
        nlohmann::json clips = nlohmann::json::array();  // ascending arrangement indices placing this pattern
        for (std::size_t c = 0; c < p.arrangement.size(); ++c)
            if (p.arrangement[c].pattern == static_cast<int>(i)) clips.push_back(static_cast<int>(c));
        nlohmann::json e;
        e["index"] = static_cast<int>(i);
        e["track"] = pat.track;
        e["track_name"] = p.tracks[static_cast<std::size_t>(pat.track)].name;  // loader guarantees the index
        if (!pat.id.empty()) e["id"] = pat.id;  // omitted entirely when unnamed (never "" or null)
        e["length_beats"] = pattern_extent(pat);  // the renderer's own loop period — never re-derived here
        e["swing"] = pat.swing;
        e["notes"] = static_cast<int>(pat.notes.size());
        e["clips"] = std::move(clips);
        arr.push_back(std::move(e));
    }
    return arr.dump(2) + "\n";
}

}  // namespace op
