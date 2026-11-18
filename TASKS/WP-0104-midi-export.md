# WP-0104 — MIDI export (`opd opj2mid`)

**Spec:** docs/spec/project-model.md §MIDI export · **Status: done** · Owner: orchestrator

## Scope
`save_midi()`: Project → SMF format 1 @480ppq (tempo track + per-op-track note tracks).

## Acceptance — evidence
- Byte-deterministic double-save (`midi.export_byte_deterministic`).
- Exact content round-trip with loader: notes `(t_beats, midi_note)`, bpm, track count
  (`midi.roundtrip_notes_exact`, `midi.roundtrip_bpm_tracks`).
- Legacy multi-pattern projects: patterns gathered by `track` field — all notes survive export
  (`midi.export_legacy_all_patterns` against demo.opj).
- CLI e2e smoke: pitched.opj→mid→opj identical shape (bpm/bars/tracks/frames; seed correctly not
  carried — MIDI has no engine seed). op_tests **91 assertions ALL PASS**, goldens 5 fixtures.

## Follow-ups noticed
- Export uses MIDI channel 0 on every track; if a consumer DAW wants channels, add per-track
  `midi_channel` to the model when a real need appears (PROPOSE only — no UI needs it yet).
- Velocity export: our notes have no velocity field; fixed 90/64 used. Ties into a future
  velocity/dynamics WP across engine+MIDI+UI.
