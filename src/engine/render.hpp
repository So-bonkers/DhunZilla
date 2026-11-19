// engine/render.hpp — offline deterministic renderer (spec/project-model.md). Pure function of (project, base_dir).
#pragma once
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

}  // namespace op
