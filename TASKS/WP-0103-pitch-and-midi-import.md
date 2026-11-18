# WP-0103 — Pitched playback + MIDI import (close the hum→MIDI→sound loop)

**Spec:** docs/spec/project-model.md §synth voice + §MIDI import · **Status: done** · Owner: orchestrator

## Scope
(a) `synth` voice kind: pitch from `note.note` (12-TET, A4=440), fundamental+octave partial,
linear A/R. (b) `opd midi2opj`: bounds-checked SMF format 0/1 reader → Project.

## Acceptance — evidence
- Zero-crossing frequency tests: A4→440Hz ±2, octave doubling within 1%, absent-note fallback.
- Legacy byte-stability: `"note"` key emitted only when present; synth keys only for synth voices
  (existing goldens untouched and still matching).
- MIDI parse: committed hand-built fixture (`tests/fixtures/midi/gen_two_notes.py` regenerable);
  ppq→beats exact, track names, vel-0-off convention, retrigger handling, first-tempo-wins.
- Truncation fuzz: every prefix of the fixture fails cleanly — no crash, no false success.
- Golden chain MIDI→opj→render: `imported.opj/.wav` in goldens; harness **5 fixtures 0 failures**;
  op_tests **81 assertions ALL PASS**.

## Handoff — round-trip metric (harness/roundtrip_eval.sh)
Engine-rendered synth melody → basicpitch ONNX → midi2opj → pitch multiset comparison:
**F1 = 1.00** (want/got both [60,60,64,64,67,67]) at threshold 0.8; exits 3 (skip) without
OP_TRANSCRIBE_PYTHON so CI-safe. This is plan §9's MIDI-accuracy metric, first instantiation.

## Follow-ups noticed
- Round-trip uses pitch multiset only; add onset/timing tolerance to the scorer when we have a
  real corpus (PROPOSE: extend into proper note-event F1 with ±50ms window in a future metrics WP).
- `load_midi` ignores CC/pitch-bend/program — fine for v0; expression import = Phase 2.
- Multi-channel MIDI tracks are collapsed per-track regardless of channel (documented); channel→track
  split belongs to the UI import dialog, not the engine.
