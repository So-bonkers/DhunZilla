# WP-0124 — prompt → editable song (end-to-end chain)

**Status: done** · Owner: orchestrator · Zero engine/sidecar changes — pure composition of shipped interfaces.

## Scope
`harness/prompt_to_song.sh "prompt" [dir]`: sidecar generate job → transcribe job → `opd midi2opj`
→ attach source audio + MIDI with `backend@model` provenance → final render. Defaults mockgen+mocktrans
(CI-safe, zero ML deps); real Basic Pitch via OP_TRANSCRIBE_PYTHON.

## Acceptance — evidence
- Mock mode: song.opj (2 attachments) + final_render.wav + ai/{source_audio.wav, transcribed.mid}
  produced; info shows `mockgen@mockgen-0.1.0` + `mocktrans@mocktrans-0.1.0`.
- **Determinism: two runs, same prompt → song.opj AND final_render.wav byte-identical** (cmp clean) —
  the whole AI chain inherits the engine's determinism contract end to end.
- Real mode: basicpitch ONNX transcribed generated audio; chain completed with
  `basicpitch@basic-pitch-nmp-onnx-ort1.32.0` provenance on the attached MIDI.

## Handoff report
**What changed:** one script + README showcase block. Module boundaries honored (HTTP API + opd CLI only).
**Note:** real-mode Basic Pitch on synthetic mockgen tones worked, but note *quality* there is a
property of the placeholder generator, not the chain — when a serious generate backend lands, its
transcription fidelity deserves its own eval harness (PROPOSE: extend roundtrip_eval with a generated-audio corpus).
**Follow-ups noticed:** prompt_to_song writes producers.txt as a human-readable sidecar; once UI exists,
attachments alone suffice and this file can go. Chain currently uses fixed duration 4.0s/seed 2026 —
expose as flags when someone asks (YAGNI now).
