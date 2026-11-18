# WP-0119 — hum→MIDI into project (+ mocktrans CI backend)

**Status: done** · Owner: orchestrator · Spec: docs/spec/backend-adapters.md (new backend), glue = composition of shipped interfaces

## Scope
`backends/mocktrans/` (deterministic canned-MIDI TranscriptionBackend, auto-discovered),
conformance check2 task-dispatch fix (same gap class as WP-0008's check5),
`harness/midi_into_project.sh`: transcribe job → copy MIDI to `<proj>/ai/transcribed.mid` →
`opd import-midi` (notes become tracks) → `opd attach --kind transcription` with backend@model.

## Acceptance — evidence
- Sidecar suite **72 passed, 2 skipped** (was 65+2): +4 mocktrans conformance matrix entries auto-
  discovered, +3 unit tests pinning MIDI byte structure. ruff + mypy --strict clean.
- CI mode: glue on gen-testproject + pitched.opj render → tracks 3→4, attachment
  `mocktrans@mocktrans-0.1.0` visible in `opd info`.
- Real mode (OP_TRANSCRIBE_PYTHON): `basicpitch@basic-pitch-nmp-onnx-ort1.32.0` — hum→editable
  notes with exact model provenance, one command.

## Handoff report
**What changed:** new backend pkg (2 files), conformance dispatch elif (1 hunk), glue script, unit
tests, README section. No C++ changes.
**Two real bugs caught en route, both by end-to-end consumption rather than contract tests:**
1. `bytes({0x90, note, 90})` — Python **set** literals are unordered; the mock emitted scrambled
   MIDI that conformance happily accepted (it checks API shape + cross-run equality, not musical
   validity). Only our own opd parser refused it. Lesson recorded in test file header: contract
   tests verify shape, harness e2e verifies semantics — you need both layers.
2. Conformance check2 hardcoded "separate" for every non-generation backend (transcription backends
   could never pass determinism) — fixed with the same isinstance dispatch as check5.
**Follow-ups noticed (PROPOSE):** a third glue variant would justify merging stem/midi scripts into
one `ai_into_project.sh`; transcription of long audio should chunk (Basic Pitch handles ~30s fine,
basicpitch backend caps at 3600s declared — worth an e2e check someday).
