// engine/tracks.cpp — WP-0227: implementation of the `opd tracks` view (contract in engine/tracks.hpp).
// The voice objects are copied verbatim from the canonical project serializer (project_to_json), so the
// file format's omit-at-default rules apply unchanged and nothing here re-implements them.
#include "engine/tracks.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "engine/model_json.hpp"

namespace op {

std::string tracks_json(const Project& p) {
    // Build the canonical serialization once: its tracks[i]["voice"] sub-object IS the canonical voice
    // text (every omit-at-default rule), and tracks[i]["voice"]["kind"] is the kind string the file writes.
    const nlohmann::json canonical = project_to_json(p);
    nlohmann::json arr = nlohmann::json::array();
    for (std::size_t i = 0; i < p.tracks.size(); ++i) {
        const Track& t = p.tracks[i];
        const nlohmann::json& vj = canonical.at("tracks").at(i).at("voice");
        nlohmann::json e;
        e["index"] = static_cast<std::size_t>(i);
        e["name"] = t.name;
        e["kind"] = vj.at("kind");  // exactly what the project file writes for this voice
        e["gain_db"] = t.gain_db;   // float32 widened to double; nlohmann shortest round-trip text
        e["pan"] = t.pan;
        e["mute"] = t.mute;
        e["solo"] = t.solo;
        e["audible"] = track_audible(p, i);  // the renderer's own mixer rule — never re-derived
        e["choke_group"] = t.choke_group;
        e["voice"] = vj;                      // canonical voice object, keys as the file format emits them
        arr.push_back(std::move(e));
    }
    return arr.dump(2) + "\n";
}

}  // namespace op
