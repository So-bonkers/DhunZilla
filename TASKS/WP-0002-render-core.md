---
id: WP-0002
title: Deterministic render core v0 (metronome/sine/sampler -> PCM16 WAV) + unit tests
phase: 0
status: done
depends_on: [WP-0001]
estimate_agent_hours: 8
human_review: demo (rendered demo.wav — LISTEN when convenient)
---

## Context / Spec
Per docs/spec/project-model.md. Headless C++20, vendored nlohmann/json only, ADR-0004 determinism flags.

## Handoff report
- **What changed:** src/engine/{model.hpp,model.cpp,wav.hpp,wav.cpp,render.hpp,render.cpp,rng.hpp,dsp.hpp}, src/cli/main.cpp (opd render/gen-testproject/info), tests/test_main.cpp, CMakeLists.txt.
- **How verified:** `./build/op_tests` → ALL PASS (0 failures), 39 checks incl. bit-identical re-render, pan/gain math, PCM16 half-to-even edges, model validation rejects. Demo audio: out/r1.wav (4s, 2 bars @120).
- **Decisions made in-flight:** hard pan = exact zeros (DAW convention); NaN→silence in PCM16 mapping documented; sampler is 48k-only in v0 with clear error.
- **Follow-ups noticed:** PCG32 cross-arch frozen vector test (needs x86 CI runner — add when pushed); block-based RT rendering path deferred (offline direct-event render only); `-ffp-contract=off` alone does NOT guarantee libm bit-exactness across glibc versions — PROPOSE: self-contained DSP sin/exp tables when cross-arch goldens first run in CI.
