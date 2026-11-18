# WP-0008 — Audio→MIDI transcription cartridge (Basic Pitch / ONNX Runtime)

**Spec:** docs/spec/backend-adapters.md + docs/adr/0005-transcription-onnx-runtime.md · **Status: done** · Owner: orchestrator
(ROADMAP rows labeled WP-0102 pre-commit refer to this package.)

## Scope
TranscriptionBackend cartridge running Spotify Basic Pitch 0.4.0's bundled nmp.onnx on onnxruntime,
via opt-in interpreter `OP_TRANSCRIBE_PYTHON` (TF rejected: no aarch64 wheels — ADR-0005).

## Acceptance — evidence
- Opt-in guard: module ImportError without env var → registry skips; CI-safe (MODE A 65 passed/2 skipped, ruff+mypy clean).
- Graceful degradation: broken interpreter → health unavailable w/ actionable reason; job fails structured (`backend_unavailable`).
- Real e2e on aarch64: WAV→MIDI through JobRunner; MThd header verified; provenance.json correct (MODE B **73 passed**, incl. conformance checks 1+5 running real transcription).
- Manifest entry `basic-pitch-onnx`: now hash_verified:true with measured sha256 of nmp.onnx from basic-pitch==0.4.0 install (first fully-pinned model in manifest).
- Conformance suite fix: check 1 + check 5 previously ignored transcribe-family tasks entirely — now probe them properly (real gap found by the first transcription backend).

## Follow-ups noticed
- MIDI F1 metric harness (plan §9) still open — needs reference corpus; natural next package.
- Runtime-pack sizing measured: ~595 MB ONNX venv → feeds §6b pack definitions.
- hum→MIDI product flow = basicpitch + engine sine/sampler playback; UI glue in Phase 1+.
