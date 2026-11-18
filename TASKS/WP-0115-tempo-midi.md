# WP-0115 — Tempo map ⇄ MIDI

**Spec:** docs/spec/project-model.md §Tempo map ⇄ MIDI · **Status: done** · Owner: orchestrator

## Scope
save_midi writes FF51 per tempo_map point; load_midi collects all FF51s → tempo_map when ≥2 distinct.
Out of scope: ritardando/ramp smoothing (piecewise-constant only), SMPTE divisions, MIDI clock sync.

## Acceptance — evidence
- `tmid.two_ff51` / `tmid.tempo_values`: byte-level walk of exported track 0 finds FF51 at ticks
  0 and 7680 (=bar 4) with 120/72 BPM values.
- `tmid.map_restored`: re-import yields 2-point map, bar 4 exact, bpm within µs quantization (<1e-3).
- `tmid.empty_and_single_point_identical`: empty map ≡ {0,bpm} export bytes → legacy callers unchanged.
- `tmid.single_tempo_imports_legacy`: fixture (one tempo) imports with EMPTY map — imported.opj golden safe.
- Goldens 6/6; harness roundtrip_eval F1 1.00/1.00 after the import-path change; op_tests **149 ALL PASS**.

## Handoff report
**What changed:** save_midi track-0 block (single→multi FF51), load_midi tempo collection
(tempo_seen flag → vector + post-parse map build). No other files touched.
**How verified:** byte-walk assertions on export, structural on import, plus regression guards for
both legacy paths (export bytes, empty map) and the full AI round-trip harness.
**Metrics:** 149 assertions (+8). Zero new deps; 2 interface files (midi.cpp only — hpp unchanged).
**Follow-ups noticed (PROPOSE):**
- import_midi_into ignores the source's tempo_map when merging into a host with its own map —
  correct for grid semantics but worth an explicit warning line in CLI output.
- A ritardando expressed as many FF51s within one bar collapses to first-per-tempo-change; fine,
  but UI should show the raw curve if we ever display tempo lanes.
