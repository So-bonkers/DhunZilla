# WP-0117 — `opd info` for .mid

**Status: done** · Owner: orchestrator · Spec: covered by docs/spec/midi.md import rules (display only)

## Evidence
- Fixture: `bpm=120.00 tracks=2 notes=5` + per-track lines (`Lead: 4`, `Bass: 1`).
- Multi-tempo export file shows `(tempo map)` marker.
- No engine change — thin CLI over load_midi; op_tests still ALL PASS (166).

## Follow-ups noticed
- `.MID` uppercase handled; other extensions fall through to project parsing with a clear error.
