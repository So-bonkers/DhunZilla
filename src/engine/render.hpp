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

// Bar -> beat on the project's beat timeline (WP-0210): the single definition of bar numbering.
// Bars are 0-based and fractional bars are allowed; the result is bar * beats_per_bar exactly.
// Example: beats_at_bar(p, 4.0) == 16.0 for a 4/4 project (p.beats_per_bar == 4).
double beats_at_bar(const Project& p, double bar);

// Beat -> sample in the PROJECT's sample rate via the renderer's own tempo map (WP-0210), i.e.
// TempoMap::at(beat) for build_tempo_map(p, p.sample_rate). Unlike sample_at_bar this is NOT
// clamped to total_samples_for(p): a ruler may draw positions past the last bar. Negative beats
// are not supported. Example: sample_at_beat(p, 20.0) == 544000 for tests/fixtures/goldens/tempo.opj.
long long sample_at_beat(const Project& p, double beat);

// Tempo in BPM in effect at `beat` (WP-0210): p.bpm when the project has no tempo_map, else the
// bpm of the last tempo point whose bar * beats_per_bar <= beat. Example: for
// tests/fixtures/goldens/tempo.opj (72 bpm from bar 4) bpm_at_beat(p, 15.75) == 120.0 and
// bpm_at_beat(p, 16.0) == 72.0.
double bpm_at_beat(const Project& p, double beat);

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

// Arrangement clip view (WP-0199, docs/spec/operations.md §Arrangement clip view): the exact text
// `opd clips <proj.opj>` prints — one JSON entry per clip in p.arrangement order (never sorted),
// the stored clip fields plus start/end in beats and samples computed with THIS renderer's own
// tempo map at p.sample_rate. Pure function of the model: no I/O, no clock, no randomness; it
// writes nothing and modifies nothing. Empty/absent arrangement → exactly "[]\n" (the implicit
// legacy placements are not synthesized). Values are unclamped: a clip may extend past length_bars
// and its true mapped position is reported. Example:
//   const std::string text = op::clips_json(p);  // "[\n  {\n    \"end_beats\": 22.0,\n ... ]\n"
std::string clips_json(const Project& p);

}  // namespace op
