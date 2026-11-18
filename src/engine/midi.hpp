// engine/midi.hpp — minimal Standard MIDI File reader → Project (WP-0103b).
// Supports SMF format 0/1, ticks-per-quarter-note division only (SMPTE rejected),
// note on/off + running status, tempo meta (first tempo wins → project bpm), track-name
// meta. Everything else is skipped with correct byte accounting. Notes are sorted by
// (t_beats, midi_note) so output never depends on event interleaving — determinism first.
#pragma once
#include <string>

#include "engine/model.hpp"

namespace op {

// Reads *path* as an .mid file and fills `out` per the rules above.
// sample_rate fixed 48000; seed/bpm default (120) unless tempo meta found; beats_per_bar 4.
// Returns false + actionable err on any structural violation (never partial success).
bool load_midi(const std::string& path, Project& out, std::string& err);

// Writes *p* as SMF format 1 @ 480 ppq: track 0 = tempo (+4/4 meta), one track per op track
// (name meta, notes sorted by (t_beats, midi_note)). Notes without `midi_note` fall back to
// round(voice.freq_hz) in 12-TET nearest-number mapping. Beat→tick uses llround; deltas are
// clamped non-negative so output is always well-formed. Byte-deterministic for a given Project.
bool save_midi(const std::string& path, const Project& p, std::string& err);

}  // namespace op
