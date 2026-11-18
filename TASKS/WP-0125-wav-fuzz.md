# WP-0125 — WAV reader fuzzing & hardening

**Status: done** · Owner: orchestrator · Spec: docs/spec/project-model.md (sampler input contract tightened below)

## Scope
`wav::read_wav` consumes files from hostile .opd bundles. Crafted fmt mutations (tag/bits/channels/sr)
+ 400-trial seeded structural fuzz. Acceptance: never crash, never OOB, reject invalid cleanly.

## Findings — evidence
Pre-fix, **4 crafted float-tag confusions were ACCEPTED with insane samples** (CHECK failures):
- tag=3 + bits∈{8,16,24}: reader computed bps from bits but always consumed 4 bytes/element →
  garbage floats; bits=24 reads ~33% past the data region (OOB read on a truncated file).
- tag=3 + bits=0: `data_len / 0` — **silent zero on aarch64** (integer div-by-zero does NOT trap
  there!) → "valid" empty sample set instead of a crash. Platform trivia worth knowing: we saw no
  SIGFPE because ARM swallows it; x86 CI would have died loudly.
Post-fix (`bits==32` required for tag 3, `sr ∈ (0, 1e6]`): all crafted cases rejected cleanly
(must_reject asserted per case), random fuzz survives with sane samples only, **ASAN+UBSAN clean**,
op_tests **192 ALL PASS**, goldens 6/6 bit-exact.

## Handoff report
**What changed:** wav.cpp +3 validation lines; test_main.cpp +wf suite (~90 lines); ROADMAP.
**Pattern established:** untrusted-input readers need (a) strict field-domain validation up front,
(b) crafted-mutation tests per field, (c) random structural fuzz — all three now exist for MIDI,
JSON, WAV, and bundle CRC. The .opd attack surface is covered end to end.
**Follow-ups noticed:** sidecar's Python wav loader (used by transcribe/demucs input validation)
has its own parser in audio_io(?) — same field domains apply; PROPOSE a mirrored crafted-case test
there when convenient (it feeds librosa/basicpitch, which mostly tolerate garbage, but errors should
be clean at the boundary).
