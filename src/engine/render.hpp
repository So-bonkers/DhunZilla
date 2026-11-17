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

}  // namespace op
