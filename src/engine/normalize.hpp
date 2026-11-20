// engine/normalize.hpp — plan and apply a loudness gain: reach a target integrated LUFS without the
// sample peak exceeding a ceiling (docs/spec/normalize.md, WP-0169). Reuses op::analyze unchanged.
//
// Example:
//   std::vector<float> v = /* interleaved float samples */;
//   op::normalize::GainPlan plan;
//   std::string err;
//   if (op::normalize::plan_gain(v, 2, 48000, -16.0, -1.0, plan, err))
//       op::normalize::apply_gain_db(v, plan.gain_db);  // v now measures ~-16 LUFS, peak <= -1 dBFS
#pragma once
#include <string>
#include <vector>
namespace op::normalize {
struct GainPlan {
    double input_lufs = 0.0;         // integrated loudness of the input (op::analyze)
    double input_peak_dbfs = 0.0;    // sample peak of the input (op::analyze peak_dbfs)
    double requested_gain_db = 0.0;  // target_lufs - input_lufs
    double gain_db = 0.0;            // the gain to apply
    bool limited = false;            // true iff the ceiling reduced the gain
};
bool plan_gain(const std::vector<float>& interleaved, int channels, int sample_rate, double target_lufs,
               double ceiling_dbfs, GainPlan& plan, std::string& err);
void apply_gain_db(std::vector<float>& samples, double gain_db);
}  // namespace op::normalize
