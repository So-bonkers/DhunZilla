// engine/dsp.hpp — the ONLY place engine code may touch libm (AGENTS.md determinism rule).
// All wrappers operate on float32. No intrinsics, no fast-math. See ADR-0004.
#pragma once
#include <cmath>

namespace op::dsp {

inline float kPi() { return 3.14159265358979323846f; }
inline float twoPi() { return 2.0f * kPi(); }

// Linear gain from dBFS. 0 dB -> 1.0, -6.0206 dB ~= 0.5.
inline float db_to_lin(float db) { return std::pow(10.0f, db * (1.0f / 20.0f)); }

// Equal-power pan: pan in [-1, 1]; returns left/right gains with L^2+R^2 == 1.
// Hard pans are exact zeros (DAW convention; avoids denormals and guarantees mono-clean stereo).
inline void equal_power_pan(float pan, float& left, float& right) {
    if (pan <= -1.0f) { left = 1.0f; right = 0.0f; return; }
    if (pan >= 1.0f) { left = 0.0f; right = 1.0f; return; }
    const float theta = (pan + 1.0f) * 0.25f * kPi();  // 0..pi/2
    left = std::cos(theta);
    right = std::sin(theta);
}

inline float sinf_(float x) { return std::sin(x); }
inline float cosf_(float x) { return std::cos(x); }
inline float expf_(float x) { return std::exp(x); }
inline float sqrtf_(float x) { return std::sqrt(x); }

inline float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

}  // namespace op::dsp
