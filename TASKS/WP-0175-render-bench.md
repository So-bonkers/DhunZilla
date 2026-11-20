---
id: WP-0175
title: tools/render_bench.py - render benchmark and determinism soak for opd
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
---

## Context - why this exists
ADR-0004 says the same project and seed render to byte-identical audio, but only the golden set exercises
that, and nothing measures how fast `opd render` is (the realtime factor decides when we need the
performance work in the plan). This package adds a small stdlib-only dev tool that renders a project N times
with a given `opd`, times each run, hashes every output, and reports one JSON document; it fails if the runs
disagree. It is tooling, not engine code: no C++ change, wall-clock time is allowed here.

## Spec - exact interface
New files only: `tools/render_bench.py` and `tools/test_render_bench.py` (create the `tools/` directory).

```
python3 tools/render_bench.py --opd PATH PROJECT.opj [--runs N] [--seed S]
        [--json OUT.json] [--expect-sha256 HEX] [--min-realtime F]
```
- Pure Python 3 stdlib (`argparse, hashlib, json, os, statistics, struct, subprocess, sys, tempfile, time`).
  Exposes `main(argv: list[str] | None = None) -> int` and `wav_info(data: bytes) -> tuple[int, int, int]`
  (sample_rate, channels, frames).
- `--runs` default 5, must be >= 1. Each run: `<opd> render <project> -o <tmpdir>/r<i>.wav` plus
  `--seed <S>` only when `--seed` was given; time it with `time.perf_counter()` around the subprocess; read
  the output file; sha256 it; parse it with `wav_info`. Temp files live in a `tempfile.TemporaryDirectory`
  that is removed afterwards.
- `wav_info`: parse RIFF/WAVE chunks (skip unknown chunks, honour the odd-size pad byte), take the `fmt `
  chunk's channels/rate/block-align and the `data` chunk length; frames = `data_len // block_align`. Do NOT use
  the `wave` module (it rejects float WAVs). Raise `ValueError` for non-WAV input or a missing chunk.
- stdout (always, when all runs completed): one JSON object (`indent=2`, `sort_keys=True`, trailing newline)
  with exactly these keys: `project`, `runs`, `seed`, `sample_rate`, `channels`, `frames`, `bytes`,
  `audio_seconds` (frames/rate), `wall_seconds` (list of N floats), `min_s`, `median_s`, `max_s`,
  `realtime_factor` (audio_seconds / median_s, null if median is 0), `sha256` (of run 0), `distinct_outputs`
  (number of distinct hashes), `deterministic` (distinct_outputs == 1). `--json OUT` also writes the same text.
- Exit codes: `0` ok; `1` nondeterministic (stderr line `NONDETERMINISTIC: <k> distinct outputs in <N> runs`)
  or `--expect-sha256` differs from run 0's hash (stderr `GOLDEN MISMATCH: expected <hex>, got <hex>`);
  `2` usage error (missing project file, `--opd` not an executable file, `--runs` < 1, argparse errors) with
  stderr starting `error:` for the three explicit cases; `3` opd exited non-zero (stderr
  `error: render failed (exit <code>): <first 300 chars of opd stderr>`) or its output is not a readable WAV
  (stderr `error: <reason>`); `4` `--min-realtime F` given and `realtime_factor` is null or `< F` (stderr
  `TOO SLOW: realtime factor <x> < <F>`). Check order after the report is printed: 1, then 4.
- `tools/test_render_bench.py`: stdlib `unittest`, at least 6 tests, using a fake `opd` (a small Python script
  written to a temp dir with the exec bit that writes a WAV; env var switches it to nondeterministic,
  failing and junk-output modes). Cover `wav_info` (PCM, an extra chunk before `data`, junk input), the report
  keys, nondeterminism -> 1, opd failure -> 3, usage -> 2. Use a unique temp dir per test (include the pid).
  Runnable as `python3 -m unittest discover tools`.

## Acceptance criteria
- [x] Everything above implemented; `python3 -m unittest discover tools` passes.
- [x] Real run: with a built `opd` from this repo, `python3 tools/render_bench.py --opd <build>/opd
      tests/fixtures/goldens/demo.opj --runs 3` exits 0 with `"deterministic": true`; paste its output.
- [x] Nothing else in the repo changes (default C++ gates are untouched; do not rebuild just for this beyond
      the real run above).

## Out of scope - do NOT touch
- `ROADMAP.md`, `src/`, `tests/`, `harness/`, `.github/`, `docs/`, `ai-sidecar/`, `CMakeLists.txt`.
- No CI wiring, no memory/CPU profiling, no multi-process soak, no golden updates.

## Git
- Branch `wp-0175-render-bench` from `main`. Commit: `WP-0175: tools/render_bench.py render benchmark + determinism soak`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:** New files only: `tools/render_bench.py` (render benchmark + determinism soak, stdlib-only,
  exposes `main(argv)` and `wav_info(data)`) and `tools/test_render_bench.py` (12 unittest tests with a fake
  `opd` script; `FAKE_OPD_MODE` env var switches it to nondeterministic / failing / junk-output modes).
  No other repo file was touched.
- **How verified:**

  1) Unit tests — `python3 -m unittest discover tools`:
  ```
  ............
  ----------------------------------------------------------------------
  Ran 12 tests in 0.386s

  OK
  ```

  2) Real run — `python3 tools/render_bench.py --opd build/opd tests/fixtures/goldens/demo.opj --runs 3` (exit 0):
  ```
  {
    "audio_seconds": 4.0,
    "bytes": 768044,
    "channels": 2,
    "deterministic": true,
    "distinct_outputs": 1,
    "frames": 192000,
    "max_s": 0.004969528999936301,
    "median_s": 0.004604522000590805,
    "min_s": 0.0026371159983682446,
    "project": "tests/fixtures/goldens/demo.opj",
    "realtime_factor": 868.7112363643308,
    "runs": 3,
    "sample_rate": 48000,
    "seed": null,
    "sha256": "8fafe2f99afdac535648c9d0d220ef7c4e3f89afb01edc4031fafdacb1c24d2b",
    "wall_seconds": [
      0.004969528999936301,
      0.004604522000590805,
      0.0026371159983682446
    ]
  }
  exit=0
  ```
  Note the run-0 sha256 equals the committed golden (`sha256sum tests/fixtures/goldens/demo.wav` →
  `8fafe2f99afdac535648c9d0d220ef7c4e3f89afb01edc4031fafdacb1c24d2b`), so the tool round-trips the golden set.

  3) Exit-code paths against real `opd`:
  ```
  --expect-sha256 <correct>            -> exit=0
  --expect-sha256 deadbeef             -> GOLDEN MISMATCH: expected deadbeef, got 8fafe2f9...  exit=1
  missing project file                 -> error: project file not found: nosuch.opj                exit=2
  --opd README.md (not executable)     -> error: --opd is not an executable file: README.md        exit=2
  --runs 0                             -> error: --runs must be >= 1                               exit=2
  --min-realtime 1e9                   -> TOO SLOW: realtime factor 891.083 < 1000000000.0        exit=4
  ```
  (Exit 3 — opd non-zero / junk WAV — is covered by the fake-opd tests `test_opd_failure_exit_3` and
  `test_junk_output_exit_3`; deliberately not provoked against the real binary.)

  4) `--seed 42 --json OUT` with real opd: exit 0, report contains `"seed": 42`, `"deterministic": true`,
  and the file matches stdout.

  5) Repo untouched beyond the new files — `git status --short` before commit showed only
  `?? TASKS/WP-0175-render-bench.md` and `?? tools/`. No rebuild was done; used the existing `build/opd`.

- **Metrics:** demo.opj (4.0 s audio) renders in ~2.6–5.0 ms wall on this aarch64 rig → realtime factor ≈ 870×.
- **PROPOSE / BLOCKED:** none.
