// engine/render.hpp — offline deterministic renderer (spec/project-model.md). Pure function of (project, base_dir).
#pragma once
#include <string>

#include "engine/model.hpp"
#include "engine/wav.hpp"

namespace op {

// Total render length in samples (spec §1). Deterministic from counters only.
long long total_samples_for(const Project& p);

// Renders project into `out` (stereo float, project sample_rate). base_dir resolves sampler paths.
// Returns false + err on any failure (unknown voice params resolved at parse time).
bool render_project(const Project& p, const std::string& base_dir, wav::StereoBuffer& out, std::string& err);

// Stems-first export: renders each track solo into stems[i], plus the full mix.
// Guarantee: summing float stem buffers in track order is bit-identical to `mix` whenever each
// track has at most one active voice contribution per sample (no intra-track note overlap); with
// overlaps the difference is float-associativity only (< 1e-6 abs) — stems still recombine
// losslessly for practical purposes. Caller provides sized vectors; buffers are reset by this call.
bool render_stems(const Project& p, const std::string& base_dir, std::vector<wav::StereoBuffer>& stems,
                  wav::StereoBuffer& mix, std::string& err);

}  // namespace op
