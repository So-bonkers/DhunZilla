# WP-0120 — Clip-level gain

**Spec:** docs/spec/project-model.md §Clip gain · **Status: done** · Owner: orchestrator

## Scope
`clip.gain_db ∈ [-96, 24]` (default 0) applied per arrangement clip in the renderer; parse/save rules
mirror velocity's byte-stability. Out of scope: fades within a clip (ramps), crossfades between clips.

## Acceptance — evidence
- `cg.parse` default+explicit; `cg.rejects_out_of_range` (+30 dB rejected).
- Save: emitted when set, omitted at unity — asserted on the serialized arrangement array only
  (first attempt caught Track's own gain_db field polluting a naive text search — test bug, fixed
  by bracket-scoping the slice; product output was correct all along).
- Gain law: −6 dB clip quarters window energy vs unity twin (±0.02); untouched clip region renders
  bit-identical between projects (`cg.untouched_clip_bit_identical`).
- Legacy: all 6 goldens bit-exact (arranged.opj predates the field).
- op_tests **176 ALL PASS**, warning-free build.

## Handoff report
**What changed:** model.hpp/cpp (field+parse+save), render.cpp (lambda param + copy block), tests,
spec, ROADMAP. ~40 lines total; same proven copy-the-mix-target pattern as velocity — no voice code
touched, cost is zero at unity.
**Follow-ups noticed (PROPOSE):** per-clip pan could ride the same mechanism if demand appears;
fade-in/out ramps need a new Note-independent envelope concept — sizeable, file when UI needs it.
