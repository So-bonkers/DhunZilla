# ADR-0004: Cross-architecture bit-exact determinism rules

**Status:** Accepted (autonomous) · 2026-11-18

## Decision
Engine builds compile with `-ffp-contract=off`, no fast-math, libm access only through
`src/engine/dsp.hpp` wrappers; RNG is a hand-written PCG32 (fixed algorithm, seeded per render);
transport derives from sample counters only. Golden renders must be bit-identical run-to-run and
byte-identical across x86_64 and aarch64 CI runners.

## Context
Golden-file testing is worthless if the engine isn't reproducible; float contraction (FMA) and
vendor SIMD are the classic cross-arch divergence sources. Agents cannot debug "sounds slightly
different", so determinism is enforced at compile level, not by convention.

## Consequences
- Possible minor perf loss vs FMA — irrelevant for offline v0 renders; revisited per-subsystem with benchmarks + ADR if it ever matters on the RT path.
- Any future use of vendor intrinsics (e.g. NEON/SSE DSP kernels) requires its own ADR with cross-arch bit-exact proof in CI.
