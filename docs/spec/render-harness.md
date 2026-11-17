# Spec: Golden-File Harness

The harness is the project's immune system: it turns "sounds right?" into exit codes.

## Tools
`harness/golden_diff.py <a.wav> <b.wav> [--mode bitexact|dbfs] [--tol-dB X]`
- `bitexact`: byte-for-byte after stripping nothing (canonical header included). Exit 0/1.
- `dbfs`: reports per-channel RMS dBFS delta, peak, max abs sample delta; fails if max-abs > tol (default 1e-7).
- Prints one machine-parsable line: `RESULT pass|fail mode=... max_abs_delta=... rms_db_a=... rms_db_b=...`

`harness/run_goldens.sh` — for every fixture pair in `tests/fixtures/goldens/` (`*.opj` + expected `*.wav`):
render twice with `opd render`, assert bit-exact run-to-run, then diff vs golden. Any failure → exit 1 with which fixture failed.

## Rules
- Golden WAVs are committed under `tests/fixtures/goldens/`. Regenerating a golden requires the work package to say so explicitly, and the PR must show the before/after dBFS diff (AGENTS.md).
- The harness itself is pure-python stdlib + numpy (no torch) — it must run on any CI runner in seconds.
- Determinism contract under test: same binary, two renders → identical; golden pair → identical across x86_64 and aarch64 runners (float determinism guard).
