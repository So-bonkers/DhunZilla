---
id: WP-0174
title: Stem bundle contract - stems.json beside every finished separate job
phase: 3
status: ready
depends_on: [WP-0004]
estimate_agent_hours: 1.0
human_review: none
---

## Context - why this exists
A `separate` job returns a dict of WAV paths. To import a split as aligned tracks the app/engine must open
every file to learn format, length and levels, and nothing stops a backend returning stems of different
lengths. Phase 3's "split any song" flow needs one machine-readable, validated description. This package
adds `stems.json` (the "stem bundle") written and validated by the job runner for every done `separate` job.
The normative text is the new file `docs/spec/stem-bundle.md` (already in the repo; read it, do not edit it).

## Spec - exact interface
All paths below are under `ai-sidecar/`.

`src/ai_sidecar/stem_bundle.py` (new module; stdlib + numpy + existing sidecar modules only)
```python
BUNDLE_FILENAME = "stems.json"
BUNDLE_SCHEMA = 1
def build_stem_bundle(artifacts: dict[str, Path]) -> dict[str, Any]: ...
def write_stem_bundle(out_dir: Path, artifacts: dict[str, Path]) -> Path: ...
```
- `build_stem_bundle`: top-level keys exactly `schema, sample_rate, channels, frames, stems`; `stems` sorted by
  name, each exactly `name, file, sha256, frames, peak, rms` as in the spec (use `ai_sidecar.wav.read_wav`; define a private streaming `_sha256(path)` helper in `stem_bundle.py`, 1 MiB chunks, do not import from `model_manager`). `peak`/`rms` are Python
  floats rounded to 6 decimals; `rms` is computed in float64 over all samples of all channels.
- Errors are `ai_sidecar.base.InvalidRequest` with exactly: `separate job produced no stems` (empty dict);
  `stem '<name>' is not a readable WAV: <underlying error>` (catch `OSError`/`ValueError` from `read_wav`);
  `stem '<name>' has (sample_rate, channels, frames) = <tuple>, expected <tuple>` where the tuples print as
  Python tuples `(44100, 2, 1000)` and the "expected" one is the first (alphabetically smallest) stem's.
- `write_stem_bundle`: `json.dumps(bundle, indent=2, sort_keys=True) + "\n"` to `<out_dir>/stems.json`,
  return the path.

`src/ai_sidecar/jobs.py` (small edit, `finalize_done` only)
- Import `write_stem_bundle`. In `finalize_done`, after the artifact-existence loop and before
  `provenance.json` is written: when `record.task == "separate"`, call
  `write_stem_bundle(record.out_dir, job_result.artifacts)` (an `InvalidRequest` it raises already becomes a
  failed job with code `inference_failed`, `retryable: false` via the caller - do not catch it). After
  `record.result` is built, add `record.result["stems_file"] = str(<path>)` for separate jobs only.
  `generate`/`transcribe` jobs are unchanged (no bundle, no `stems_file`). Do not touch anything else in
  `jobs.py`.

`tests/test_stem_bundle.py` (new): at least 6 tests, mock backends / small synthetic WAVs written with
`ai_sidecar.wav.write_wav`, no network: bundle content and sort order for a 3-stem fake; sha256 equals the
file's; peak/rms of a known sine and of silence; each of the three error messages; an end-to-end `mocksep`
job over `TestClient` (result has `stems_file`, the bundle lists `bass, drums, other, vocals`, two runs give
identical bytes); a `mockgen` job has no `stems_file` and no `stems.json`.

## Acceptance criteria
- [x] Everything above implemented; existing sidecar tests unchanged and green
      (`cd ai-sidecar && UV_CACHE_DIR=<workspace>/.cache/uv uv run pytest -q`), `uv run ruff check .` clean.
- [x] New tests as above.

## Out of scope - do NOT touch
- `ROADMAP.md`, `src/` (C++), `tests/` (repo root), `harness/`, `.github/`, `docs/` (spec already added),
  `ai-sidecar/backends/`, `app.py`, `base.py`, `model_manager.py`, `cli.py`.
- No reconstruction-error metric, no loudness, no change to backend interfaces or other result keys.

## Git
- Branch `wp-0174-stem-bundle` from `main`. Commit: `WP-0174: stem bundle (stems.json) contract for separate jobs`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - `ai-sidecar/src/ai_sidecar/stem_bundle.py` (new, 153 lines incl. docstrings): `BUNDLE_FILENAME`,
    `BUNDLE_SCHEMA`, `build_stem_bundle()`, `write_stem_bundle()` and a private streaming `_sha256()`
    (1 MiB chunks, defined here — nothing imported from `model_manager`). Levels via `_levels()`:
    peak = max |sample|, rms = sqrt(mean(sample²)) accumulated in float64, both `round(x, 6)` Python floats.
    Stems read in name order; the first defines the shared `(sample_rate, channels, frames)`. Errors are
    exactly the three `InvalidRequest` messages from the spec. Stdlib + numpy + `ai_sidecar.base`/`ai_sidecar.wav` only.
  - `ai-sidecar/src/ai_sidecar/jobs.py`: one import + `finalize_done` only — after the artifact-existence
    loop and before `provenance.json`, `record.task == "separate"` gets
    `write_stem_bundle(record.out_dir, job_result.artifacts)` (its `InvalidRequest` propagates to the
    existing contract-violation handler); `record.result["stems_file"]` is added for separate jobs only.
    Nothing else in the file touched; `generate`/`transcribe` results unchanged.
  - `ai-sidecar/tests/test_stem_bundle.py` (new, 10 tests): bundle content + exact key sets + sort order for a
    3-stem fake, sha256 == file digest, peak/rms of a crest-aligned sine (exact 0.5 / 0.353553), of a smooth
    1 kHz sine over 100 periods and of silence, byte-stability/format of `write_stem_bundle`, each of the three
    error messages, end-to-end `mocksep` over `TestClient` (result `stems_file`, four mock stems, hashes match
    the artifacts), two runs → identical bytes, and `mockgen` → no `stems_file` and no `stems.json`. No network.
  - `docs/spec/stem-bundle.md` committed unchanged as installed by the runner (no edits).
- **How verified:** (all commands run in `/home/shubhankar_dgx/qwen-scratch/openpattern/ai-sidecar` with
  `export UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`)
  - [x] Ruff clean:
    ```
    $ uv run ruff check .
    All checks passed!
    ```
  - [x] Full sidecar suite green (new file contributes the last 10 tests):
    ```
    $ uv run pytest -q
    119 passed, 2 skipped, 77 warnings in 14.78s
    $ uv run pytest -q --ignore=tests/test_stem_bundle.py     # pre-existing suite, unchanged
    109 passed, 2 skipped, 71 warnings in 14.65s
    ```
  - [x] New tests individually:
    ```
    $ uv run pytest -v tests/test_stem_bundle.py
    tests/test_stem_bundle.py::test_bundle_content_and_sort_order PASSED     [ 10%]
    tests/test_stem_bundle.py::test_sha256_matches_file_bytes PASSED         [ 20%]
    tests/test_stem_bundle.py::test_peak_and_rms_of_known_sine_and_silence PASSED [ 30%]
    tests/test_stem_bundle.py::test_write_stem_bundle_is_byte_stable PASSED   [ 40%]
    tests/test_stem_bundle.py::test_error_no_stems PASSED                     [ 50%]
    tests/test_stem_bundle.py::test_error_not_a_readable_wav PASSED           [ 60%]
    tests/test_stem_bundle.py::test_error_mismatched_stem_shape PASSED        [ 70%]
    tests/test_stem_bundle.py::test_mocksep_job_writes_validated_bundle PASSED [ 80%]
    tests/test_stem_bundle.py::test_mocksep_two_runs_identical_bundle_bytes PASSED [ 90%]
    tests/test_stem_bundle.py::test_mockgen_job_has_no_bundle PASSED          [100%]
    ======================== 10 passed, 7 warnings in 0.36s ========================
    ```
  - [x] Live `mocksep` job through the HTTP app (throwaway script, deleted afterwards) — result gained only
    `stems_file`; bundle is sorted, indent=2, trailing newline:
    ```
    === mocksep separate job ===
    state: done | result keys: ['artifacts', 'provenance_file', 'raw_dir', 'stems_file']
    stems_file: 4e90197bf0a8/stems.json
    --- stems.json (verbatim) ---
    {
      "channels": 2,
      "frames": 44100,
      "sample_rate": 44100,
      "schema": 1,
      "stems": [
        {"file": "bass.wav", "frames": 44100, "name": "bass", "peak": 0.446266, "rms": 0.030335,
         "sha256": "0bda04a9847d25f5ae6dd6449c95ab2c79109d947d81215b371c81236d8ab6a3"},
        ... drums (peak 0.448451, rms 0.089539), other (0.228398 / 0.029935), vocals (0.401399 / 0.256125) ...
      ]
    }
    ```
    (abridged here: entries are one-per-line in the file; all four stems + hashes shown by the script)
  - [x] Contract violation really fails the job as specified — a throwaway backend returning stems of
    different lengths:
    ```
    === raggedsep (stems of different lengths) ===
    state: failed
    error: {"code": "inference_failed", "message": "backend broke its contract: stem 'drums' has
            (sample_rate, channels, frames) = (44100, 2, 500), expected (44100, 2, 1000)", "retryable": false}
    ```
  - [x] `mypy --strict` on the touched modules (repo convention; not in CI):
    ```
    $ uv run mypy --strict src/ai_sidecar/stem_bundle.py src/ai_sidecar/jobs.py
    Success: no issues found in 2 source files
    ```
- **Metrics:** +1 module (153 lines incl. docstrings), +1 test file (225 lines, 10 tests), `jobs.py` +11/-0
  lines; suite 109 → 119 passing with the same 2 skips. The pytest warning count goes 71 → 77 only because the
  new tests create more apps, each re-emitting the pre-existing "demucs/basicpitch adapter disabled"
  discovery warnings — no new warning kinds. Finalizing a separate job now costs one extra read pass per stem.
- **PROPOSE / BLOCKED:** none blocked.
  - PROPOSE: `finalize_done`'s docstring still says only "Write the mandatory provenance.json" — worth a
    one-line update in a future package (left alone here because the WP said not to touch anything else in `jobs.py`).
  - PROPOSE: the engine/app importer can rely on `stems_file` being absolute — it derives from
    `create_app(jobs_dir=...)`, which the CLI/launcher passes as an absolute path.
