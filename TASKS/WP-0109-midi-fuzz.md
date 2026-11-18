# WP-0109 — MIDI mutation fuzzing

**Spec:** docs/spec/project-model.md §MIDI import (amended) · **Status: done** · Owner: orchestrator

## Scope
Seeded PCG32 byte-flip fuzzer over the MIDI fixture inside op_tests; contract = never crash/hang,
and any mutant that still parses must satisfy model invariants (pitch 0..127, dur>0, bpm>0).

## Acceptance — evidence
- Found real bug #1 on first run: note-on/off data bytes ≥ 0x80 were accepted as pitches →
  out-of-range `midi_note`. Fixed with lenient skip (event ignored, parse continues) in both handlers.
- Post-fix: 500/500 trials complete, 196 mutants parse cleanly, zero invariant violations, no crashes.
- op_tests **102 assertions ALL PASS**; goldens 6 fixtures unchanged.

## Follow-ups noticed
- Larger seed corpus + longer runs belong in a periodic (nightly) job once CI exists — same binary,
  bigger trial count via env knob if we ever want it.
