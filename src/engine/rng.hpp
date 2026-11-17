// engine/rng.hpp — PCG32 (PCG-XSH-RR 32/64), pure 64-bit integer arithmetic.
// Portable by construction: no floats, fixed constants → identical sequences on all arches.
// Usage:
//   op::rng::Pcg32 r(42);          // deterministic per render seed
//   uint32_t v = r.next();         // raw 32-bit
//   float f = r.next_float01();    // [0, 1)
#pragma once
#include <cstdint>

namespace op::rng {

class Pcg32 {
public:
    explicit Pcg32(uint64_t seed, uint64_t stream = 54u) : state_(0), inc_((stream << 1u) | 1ull) {
        (void)next();
        state_ += seed;
        (void)next();
    }

    uint32_t next() {
        const uint64_t old = state_;
        state_ = old * kMul + inc_;
        const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    float next_float01() {
        // Top 24 bits → exact binary fraction in [0,1), no rounding surprises.
        return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f);
    }

private:
    static constexpr uint64_t kMul = 6364136223846793005ull;
    uint64_t state_;
    uint64_t inc_;
};

}  // namespace op::rng
