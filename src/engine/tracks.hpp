// engine/tracks.hpp — WP-0227 (docs/spec/operations.md §Channel rack / mixer track view): the read-only
// channel-rack / mixer track view behind `opd tracks` — one JSON entry per track in model order, so the
// future panel and the copilot read track state instead of parsing .opj JSON themselves. Adds no model
// field, no operation, and changes no render. Example:
//   const std::string text = op::tracks_json(p);  // "[\n  {\n    \"audible\": true,\n ... ]\n"
#pragma once
#include <string>

#include "engine/model.hpp"

namespace op {

// The `opd tracks` view of a project: a JSON array built with nlohmann, dumped as dump(2) (two-space
// indent; object keys sort alphabetically) plus exactly one trailing '\n'. Pure function — no I/O, no
// clock, no randomness; two calls on the same Project return identical bytes. A Project with no tracks
// (only constructible in memory; the loader rejects empty projects first) returns exactly "[]\n".
//
// Each entry has EXACTLY these ten keys, always present (defaults print as themselves — unlike the
// project file, this view does not omit default mixer fields): index (position in Project::tracks),
// name (stored track.name), kind (the voice kind string exactly as the project file writes it:
// "sampler" | "synth" | "metronome" | "sine"), gain_db and pan (stored values), mute and solo (stored
// booleans), audible (= op::track_audible(p, i) — the renderer's own mixer rule, never re-derived),
// choke_group (stored value, 0 = none), and voice — the voice object byte-identical to tracks[i]["voice"]
// in the canonical project serialization (op::project_to_json), so optional keys the model does not
// hold are absent, never invented. Numbers are serialized by nlohmann exactly as stored: index /
// choke_group and the integer voice fields are JSON integers; gain_db / pan / attack_ms / release_ms /
// freq_hz are float32 model values widened to double (shortest round-trip decimal).
std::string tracks_json(const Project& p);

}  // namespace op
