# Spec: AI Backend Adapter Interface (GenerationBackend / SeparationBackend / TranscriptionBackend)

Models are **cartridges, not dependencies**. Every backend implements one small Python interface;
the sidecar core never imports a model library directly. Adapters live in `ai-sidecar/backends/<name>/`.

## Common base (`ai_sidecar/base.py`)
```python
class BackendBase(ABC):
    id: str                    # e.g. "acestep-v15", "demucs-htdemucs"
    def capabilities(self) -> Capabilities: ...   # honest declaration; conformance suite verifies
    def health(self) -> HealthInfo: ...           # deps present? vram tier? graceful, never raises
```

`Capabilities` (pydantic model) — flags: `text2song, text2loop, repaint, extend, cover, score2audio, vocal_edit, stem_separation, transcription`; plus `vram_tiers: dict[str, "ok"|"tight"|"no"]` keyed `"cpu","6gb","12gb","24gb"`; `max_duration_s`.

## SeparationBackend
```python
def separate(self, audio_path: Path, *, stems: list[str] | None = None,
             opts: dict) -> JobResult   # JobResult.artifacts = {stem_name: wav_path}, provenance set
```
Default stems: `["vocals","drums","bass","other"]`. Output: 44.1 kHz stereo float32 WAVs into the job's output dir.

## GenerationBackend
```python
def generate(self, req: GenerateRequest) -> JobResult
# GenerateRequest: prompt, lyrics|None, style_tags[], duration_s, bpm|None, key|None, seed, hardware_hint
# JobResult.artifacts: {"audio": wav_path} + optional {"score": abc_or_midi_path} (YuE2-style symbolic plan) + raw artifacts dir
```

## TranscriptionBackend
```python
def transcribe(self, audio_path: Path, *, opts: dict) -> JobResult  # artifact "midi": path; note F1 against ground truth is the conformance metric
```

## Conformance suite (`ai-sidecar/tests/conformance/`) — CI runs it for EVERY backend
1. Capability honesty: declared flags match what methods actually do (mock-level probe).
2. Determinism: same request+seed → identical output bytes (for backends declaring `deterministic=True`; stochastic models exempt but must be stable given fixed seed at their own API level if they expose one).
3. Duration accuracy: generated audio within ±5% of requested duration (generation backends).
4. Graceful degradation: no GPU / missing optional dep → health() reports tier "no" and separate()/generate() raises `BackendUnavailable` with actionable message, never a traceback dump.
5. Provenance: every JobResult carries `{backend_id, model_version, seed, request_hash}` in `provenance.json`.

## Mock backends (always present, zero heavy deps — the CI floor)
- `mockgen`: deterministic sine-chord "song" from seed; declares text2loop only.
- `mocksep`: 4-band FFT split → drums/bass/vocals/other-ish stems (fake but real DSP: exercises the whole pipeline).

Real adapters (demucs, basic-pitch, acestep, yue2) implement the same interface and are gated by
manifest license tier + installed extras; absence must never break the sidecar or its tests.
