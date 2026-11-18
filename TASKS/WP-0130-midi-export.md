# WP-0130 — MIDI export flattens arrangement

**Status: done** · Owner: orchestrator · Spec: docs/spec/project-model.md §MIDI export flattens arrangement
(renumbered from my planned 0129 after the human rename commit took that number mid-round).

## Problem (breadth-pass find)
`opj2mid` exported raw patterns, ignoring `arrangement` — any arranged song lost its structure on
MIDI share: no clip placements, no repeats, no offsets. Export ≠ what you hear = interop bug.

## Fix + semantics
Export-time flattening mirrors the renderer exactly: placement at `start_bar·bpb + r·extent`,
repeats per clip; **clip gain baked into note velocities** (MIDI has no clip level), clamped 1..127.
No arrangement → legacy default placement, byte-identical output.

## Acceptance — evidence
- `ae.*` tests: legacy 2-note round-trip; flattened 8 notes; rep onsets at beats 8/10/12 = renderer
  spacing (extent 2); gain −6.02 dB → velocities ~half (loud=2 quiet=6 split); export deterministic bytes.
- **Legacy proof:** built HEAD (pre-fix) in a throwaway worktree, exported pitched.opj with both
  binaries → `cmp` clean ("LEGACY EXPORT BYTE-IDENTICAL vs HEAD").
- op_tests **207 ALL PASS**; goldens 6/6; CLI error harness OK.

## Handoff report
**What changed:** midi.cpp save_midi (placement expansion), tests, spec, ROADMAP (+ fixed the stale
"queued" status on WP-0008 — adapter shipped rounds ago: ONNX cartridge + conformance + eval corpus),
annotated superseded WP-0128-rename plan file (restored from an unexplained local deletion; decision
record preserved).
**Note for CI:** aarch64+x86_64 export determinism now covered by ae.export_deterministic locally;
matrix proof rides the existing push pipeline.
