// engine/normalize.cpp — plan (measure + cap) and apply a loudness gain. Spec: docs/spec/normalize.md
// (WP-0169). libm only via op::dsp wrappers (ADR-0004): this file calls no std math functions directly
// (+ - * / and comparisons are fine; the range checks below rely on NaN failing every comparison).

#include "engine/normalize.hpp"

#include "engine/analyze.hpp"
#include "engine/dsp.hpp"

namespace op::normalize {

bool plan_gain(const std::vector<float>& interleaved, int channels, int sample_rate, double target_lufs,
               double ceiling_dbfs, GainPlan& plan, std::string& err) {
    // Checks in spec order. A NaN bound fails: every comparison with NaN is false.
    if (!(target_lufs >= -70.0 && target_lufs <= 0.0)) {
        err = "target LUFS out of range [-70, 0]";
        return false;
    }
    if (!(ceiling_dbfs >= -30.0 && ceiling_dbfs <= 0.0)) {
        err = "ceiling out of range [-30, 0]";
        return false;
    }
    const op::analyze::Features f = op::analyze::analyze_samples(interleaved, channels, sample_rate);
    if (!f.integrated_lufs || !f.peak_dbfs) {
        err = "no measurable loudness (silent or too short)";
        return false;
    }

    plan.input_lufs = *f.integrated_lufs;
    plan.input_peak_dbfs = *f.peak_dbfs;
    plan.requested_gain_db = target_lufs - plan.input_lufs;
    const double allowed = ceiling_dbfs - plan.input_peak_dbfs;  // dB of headroom to the ceiling
    plan.limited = plan.requested_gain_db > allowed;
    plan.gain_db = plan.limited ? allowed : plan.requested_gain_db;  // may be negative (attenuate)
    return true;
}

void apply_gain_db(std::vector<float>& samples, double gain_db) {
    const float g = static_cast<float>(op::dsp::powd_(10.0, gain_db / 20.0));  // 0 dB -> exactly 1.0f
    for (float& s : samples) s = s * g;
}

}  // namespace op::normalize
