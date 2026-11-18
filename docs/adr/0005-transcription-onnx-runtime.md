# ADR-0005: Transcription runtime = ONNX via subprocess cartridge (TensorFlow rejected)

**Status:** Accepted (agent decision, reversible; human review pending)
**Date:** 2026-11-18
**Context:** WP-0008 (audio→MIDI for hum→MIDI / "convert a voice sample to an instrument's notes")

## Decision

Transcription uses **Spotify Basic Pitch 0.4.0 running its bundled ONNX export
(`nmp.onnx`, sha256 `2c3c1d14…`) on `onnxruntime`**, driven by the `basicpitch`
cartridge (`ai-sidecar/backends/basicpitch/`) through a dedicated interpreter
(`OP_TRANSCRIBE_PYTHON`) — the same subprocess-cartridge pattern as the demucs
backend. TensorFlow is **not** used, anywhere.

## Why (evidence gathered 2026-11-18 on the reference rig, linux-aarch64)

1. **TensorFlow has no aarch64 wheels.** PyPI's `tensorflow`/`tensorflow-cpu` latest
   publish only `manylinux_2_27_x86_64` / `win_amd64`; the last versions with ARM
   wheels (≤2.11, cp≤310) are EOL and incompatible with our Python floor. Our
   linux-arm64 target *is* a first-class product platform (DGX Spark dev box), so TF
   would permanently break "one core, six targets".
2. **Basic Pitch's own default runtimes on Linux** are TFLite — which also lacks
   official aarch64 wheels. ONNX is the only portably-packaged serialization.
3. **ONNX Runtime ships proper aarch64 wheels** (Microsoft-maintained) and works on
   py3.12: verified end-to-end — `basic-pitch --model-serialization onnx` and the
   library path both produce MIDI from WAV on this machine.
4. The cartridge keeps the ~600 MB inference stack *out* of the sidecar's own venv,
   matching §6b's "slim core + downloadable runtime packs" distribution model.

## Consequences

- `backends/basicpitch/` is opt-in (import guard on `OP_TRANSCRIBE_PYTHON`); CI runs
  conformance for it only when the interpreter exists — same as demucs.
- Runtime pack v0 recipe (recorded in impl.py docstring + manifest entry): py3.12,
  `onnxruntime librosa mido midiutil numpy scipy soundfile pretty_midi mir_eval matplotlib resampy`
  + `basic-pitch --no-deps`. Re-measured install footprint: ~595 MB.
- Model integrity: weights ship inside the pip wheel; the manifest pins package
  version **and** `nmp.onnx` sha256. Future upgrade = bump both, re-run conformance.
- Determinism declared `False` (ORT thread scheduling). If we ever need byte-exact
  transcription for goldens, pin ORT to single-thread intra-op in the wrapper and
  re-test — do not flip the capability flag first.
- MIDI F1 conformance hook (plan §9 metric) stays open: needs a reference corpus;
  logged as follow-up in TASKS/WP-0008 handoff.

## Rejected alternatives

- **TF CPU route**: dead on arm64 (above). Also ~2 GB venv.
- **TFLite route**: no official aarch64 wheels.
- **Pure-C++ DSP f0 (YIN/McLeod) in-engine**: genuinely attractive for the hum→MIDI
  single-line case (zero deps, deterministic, goldens-friendly) — deferred to a
  possible WP-0103; Basic Pitch gives polyphony + pitch bends now.
- **ONNX port of heavier AMT models (MR-MT/ByteDance, etc.)**: quality upgrade for
  Phase 4+, same cartridge slot fits without core changes.
