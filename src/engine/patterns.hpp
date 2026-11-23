// engine/patterns.hpp — pattern-bar listing as canonical JSON (WP-0228). Read-only view over the
// project model: one entry per pattern in Project::patterns order, effective loop length from the
// renderer's own op::pattern_extent, swing, note counts and the arrangement clip indices that place it.
#pragma once
#include <string>
#include "engine/model.hpp"

namespace op {

// Pattern-bar listing of a project as canonical JSON text (WP-0228). Pure function of the model: no
// I/O, no clock, no randomness; two calls on the same Project return identical bytes. Output is
// arr.dump(2) plus exactly one trailing '\n' (two-space indent, object keys alphabetical), where arr
// has one entry per pattern in Project::patterns order — never sorted, never filtered. Every entry
// always carries the seven keys "index" (position in Project::patterns), "track" (stored track index),
// "track_name" (p.tracks[pattern.track].name verbatim), "length_beats" (= op::pattern_extent(pattern),
// never re-derived here), "swing" (stored, default prints 0.0), "notes" (count of notes in this
// pattern) and "clips" (ascending indices into Project::arrangement whose clip.pattern == index; []
// when the pattern is unused — a legacy project without an arrangement reports [] for every entry,
// the implicit "every pattern once at bar 0" placements are never synthesized). The optional key "id"
// appears only when the stored pattern id is non-empty (never "", null or a fallback). A Project with
// no patterns returns exactly "[]\n". Caller-validated invariant (the loader enforces it):
// 0 <= pattern.track < p.tracks.size(). Example:
//   op::Project p; std::string err;
//   if (!op::load_project("song.opj", p, err)) { /* handle */ }
//   std::fputs(op::patterns_json(p).c_str(), stdout);  // byte-exact deterministic text
std::string patterns_json(const Project& p);

}  // namespace op
