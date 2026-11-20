# Spec: Stem bundle (`stems.json`)

Every `done` job with `task == "separate"` writes `out/jobs/<job_id>/stems.json` next to
`provenance.json` (WP-0174). It is the one file the app/engine needs to import the stems of a split as
aligned tracks: shared format, per-stem file, hash and levels. Code: `ai_sidecar/stem_bundle.py`.

```jsonc
{
  "schema": 1,
  "sample_rate": 44100,        // shared by all stems
  "channels": 2,               // shared by all stems
  "frames": 441000,            // shared by all stems (stems are sample-aligned)
  "stems": [                   // sorted by name
    {"name": "bass", "file": "bass.wav", "sha256": "<hex>", "frames": 441000,
     "peak": 0.412345, "rms": 0.101234}
  ]
}
```

## Rules
- Written with `json.dumps(indent=2, sort_keys=True)` plus a trailing newline: byte-stable for equal input.
- `file` is the basename inside the job directory. `sha256` is of the WAV file's bytes.
  `peak` = max |sample| over all channels, `rms` = sqrt(mean(sample^2)) over all samples of all channels;
  both rounded to 6 decimals.
- Validation (contract violations fail the job as `inference_failed`, `retryable: false`): at least one
  stem; every artifact a readable WAV (`stem '<name>' is not a readable WAV: ...`); all stems share the
  same `(sample_rate, channels, frames)` (`stem '<name>' has (sample_rate, channels, frames) = ..., expected ...`).
- The job result gains `stems_file` (absolute path of `stems.json`); existing result keys are unchanged.
  `generate` and `transcribe` jobs never get a bundle.
- Deliberately NOT here: reconstruction error vs the source mix, loudness (LUFS), stem naming beyond what
  the backend declares.
