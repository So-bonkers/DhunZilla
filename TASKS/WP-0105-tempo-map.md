# WP-0105 — Tempo map (bar-anchored piecewise tempo)

**Spec:** docs/spec/project-model.md §tempo map · **Status: done** · Owner: orchestrator

## Scope
`tempo_map` in project model + piecewise beat→sample conversion in renderer; empty = legacy.

## Acceptance — evidence
- Single-point map `{0,bpm}` renders bit-identical to no map (`tempo.single_point_map_bit_identical_to_legacy`);
  all pre-existing goldens (demo/panorama) still matching = zero drift on legacy paths.
- Displacement math verified at exact samples: 120→60 drop @bar4 puts beat-25 note at 384000+9·48000=816000,
  silence at the constant-tempo position 600000 (`tempo.note_after_drop_at_expected_samples`,
  `tempo.note_not_at_constant_tempo_position`); durations use local tempo at note start.
- Validation: bpm range, strictly-increasing bars, must-start-at-bar-0; save round-trip preserves map.
- Golden `tempo.opj/.wav` (120→72 drop @bar4 + metro arrangement): 1,024,000 frames = exact piecewise
  math; double-render bit-exact; harness **6 fixtures 0 failures**; op_tests **101 assertions**.

## Follow-ups noticed
- Notes crossing a tempo change keep start-local duration (documented simplification).
- MIDI import/export v0 ignores maps (first tempo → constant). Export could emit points when model gains them from UI.
- Time signatures (variable beats_per_bar) deliberately NOT in this map — separate WP if demand; bars were kept uniform on purpose.
