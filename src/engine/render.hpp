// engine/render.hpp — offline deterministic renderer (spec/project-model.md). Pure function of (project, base_dir).
#pragma once
#include <cstddef>
#include <string>

#include "engine/model.hpp"
#include "engine/wav.hpp"

namespace op {

// Total render length in samples (spec §1). Deterministic from counters only.
long long total_samples_for(const Project& p);

// Bar -> sample (spec "Render a bar range"). 0-based bar, fractional ok. Uses the renderer's own tempo map.
// Example: sample_at_bar(p, 1.0) == 96000 for a 120 bpm, 4/4, 48 kHz project; bars >= length_bars clamp
// to total_samples_for(p).
long long sample_at_bar(const Project& p, double bar);

// Frames [from_frame, to_frame) of `buf` copied into `out` (same sample_rate). Requires 0 <= from < to <= frames.
void slice_frames(const wav::StereoBuffer& buf, long long from_frame, long long to_frame, wav::StereoBuffer& out);

// Renders project into `out` (stereo float, project sample_rate). base_dir resolves sampler paths.
// Returns false + err on any failure (unknown voice params resolved at parse time).
bool render_project(const Project& p, const std::string& base_dir, wav::StereoBuffer& out, std::string& err);

// Stems-first export: renders each track solo into stems[i], plus the full mix. Inaudible tracks
// (spec §mixer) yield all-zero stems of full render length; master gain multiplies mix and stems.
// Guarantee: summing float stem buffers in track order is bit-identical to `mix` whenever each
// track has at most one active voice contribution per sample AND master_gain_db == 0 (the legacy
// case); with overlaps or a non-zero master the difference is float-associativity only (< 1e-6
// abs) — stems still recombine losslessly for practical purposes. Caller provides sized vectors;
// buffers are reset by this call.
bool render_stems(const Project& p, const std::string& base_dir, std::vector<wav::StereoBuffer>& stems,
                  wav::StereoBuffer& mix, std::string& err);

// Beat onsets (spec §Click track render, WP-0148): absolute sample position of every integer beat
// b = 0, 1, 2, ... via the renderer's own beat→sample mapping, while at(b) < total_samples_for(p).
// Example: 120 bpm @ 48 kHz → {0, 24000, 48000, ...}. Index i in the returned vector IS beat number b.
std::vector<long long> beat_onsets(const Project& p);

// Renders a metronome click track for the whole project into `out` (spec §Click track render):
// total_samples_for(p) frames, stereo (channels bit-identical), bursts only on beats — tracks,
// patterns, arrangement, mute/solo and gains are ignored. Returns false + err like render_project.
// Example: op::wav::StereoBuffer buf; std::string err;
//          if (!op::render_click(p, buf, err)) { /* zero-length or >600s cap */ }
bool render_click(const Project& p, wav::StereoBuffer& out, std::string& err);

// Pattern mode (WP-0192, spec project-model.md §Pattern mode): build the *pattern-mode project* for one
// pattern — a copy of `p` whose arrangement is exactly one clip {pattern_index, start_bar 0.0, repeats
// loops} (gain 0, no fades) and whose length_bars = std::ceil(double(loops) * pattern_extent(pat) /
// double(beats_per_bar)), computed in exactly that order in double. Everything else is copied verbatim
// (tracks, mute/solo, bpm/tempo_map, sample rate, master gain, seed); the loop region is kept but the
// renderer does not use it. Caller-validated: requires pattern_index < p.patterns.size() and loops >= 1.
// A pattern with no length (pattern_extent == 0: no notes and no length_beats) fails with the spec's
// message (using pat.id); `out` is left untouched on failure. Example:
//   op::Project pm; std::string err;
//   if (!op::pattern_mode_project(p, 1, 3, pm, err)) { /* pattern '<id>' has no length (...) */ }
bool pattern_mode_project(const Project& p, std::size_t pattern_index, long long loops, Project& out,
                          std::string& err);

}  // namespace op
