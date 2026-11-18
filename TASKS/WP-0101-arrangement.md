# WP-0101 — Arrangement layer (clips on the timeline)

**Spec:** docs/spec/project-model.md §"Arrangement (clips)" · **Status: done** · Owner: orchestrator

## Scope
Playlist clips `{pattern, start_bar, repeats}` in project model + renderer; strictly backward-compatible
(absent `arrangement` = legacy semantics, bit-identical). Extent-based pattern-relative loops.

## Acceptance — evidence
- Model parse/validation incl. defaults + rejections (OOB pattern idx, repeats<1, negative start_bar): 61/61 assertions in op_tests.
- Legacy path bit-exact: `arr.clip_0_at_bar0_equals_legacy_bit_exact` + committed goldens demo/panorama unchanged through refactor.
- Placement semantics at exact sample math (60 BPM → 48000 spb): silence-before-clip, sound-at-bar-offset, extent-spaced repeats, unreferenced patterns silent.
- New golden fixture `arranged.opj` (A/B/A kick structure + fractional-bar bass clips + metro repeats), deterministic double-render, harness 3 fixtures.
- Bug found & fixed en route: `load_project` did not reset its out-param → dirty reuse accumulated state (caught by WP test).

## Follow-ups noticed
- Clip-level gain/velocity offsets and non-destructive slicing → later Phase-1 package.
- Multi-pattern-per-bar overlap stacking order is declaration-order (documented); consider per-track clip sort in UI layer, never engine.
