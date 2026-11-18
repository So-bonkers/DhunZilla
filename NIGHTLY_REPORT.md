# Nightly Report — Autonomous Phase-0 Build (2026-11-18, ~01:55→07:00 IST)

You said "build however much you can" — here's what exists when you wake up. **Everything below is committed and test-green on your DGX Spark (aarch64).** No decisions were made that block you; decision items are at the bottom.

## One-command verification
```bash
cd ~/qwen-scratch/openpattern
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j \
  && ./build/op_tests && ./harness/run_goldens.sh && ./harness/test_cli_errors.sh \
  && (cd ai-sidecar && uv sync -q && uv run pytest -q && uv run ruff check .)
```

## What was built (10 commits, all WP-tracked with handoff reports in TASKS/)

### Deterministic render core (C++20, GUI-free — ADR-0002 deviation from plan, justified)
- `opd` CLI: `render` / `gen-testproject` / `info`, spec'd exit codes, **`--stems-dir` native stem export**
- Voices: metronome (accented), sine, sampler; gain/pan/hard-pan; PCM16 48k canonical writer
- PCG32 RNG, libm-only-via-wrapper policy (`dsp.hpp`), `-ffp-contract=off` (ADR-0004)
- **43/43 unit tests**, incl. bit-identical re-render and *sum-of-stems == mix bit-exact*

### Golden-file harness (the "agents can't hear" answer) — working
- `run_goldens.sh`: 2 fixtures × (double-render determinism + golden match) + **negative control** (corrupt golden must be rejected — it is)
- `test_cli_errors.sh`: all 6 CLI error-path contracts pass
- The refactor to add stems export passed with goldens untouched — the regression net works

### AI sidecar v0 (Python/FastAPI, per ADR-0003 HTTP-not-gRPC yet)
- Job queue + adapter SDK (`base.py`) + `mockgen`/`mocksep` backends + **conformance suite** (5 checks × all backends: capability honesty, byte-determinism, duration ±5%, graceful degradation, provenance)
- Model manager: manifest with **license tri-state gates** (permissive / personal-use-ok / commercial-needs-contact), sha256-pinned resumable downloader, quarantine on hash mismatch, `awaiting-weights` stubs — the ACE-v2 day-0 pre-wiring is real (`acestep-next` entry)
- Lifecycle: spawn → `SIDECAR_PORT=` announce → health backoff (race found & fixed)
- **59/59 tests green, ruff clean, mypy --strict clean**

### Real AI engine integrated (WP-0006 — done, not just verified)
- **Demucs htdemucs runs on your box: CPU-only, 1.57× realtime.** Now a real sidecar cartridge (`backends/demucs/`): opt-in via `OP_DEMUCS_PYTHON` (torch stays out of the sidecar core), full conformance suite coverage when enabled, **real 4-stem separation end-to-end through the `/jobs` API tested on this machine** — 66 tests green in "real" mode, 62+skip green in CI mode without torch.
- Install gotchas recorded (`../exp-demucs/`): needs `CMAKE_POLICY_VERSION_MINIMUM=3.5` for sphn + explicit numpy. This is packaging intel for §6b runtime-packs.

## Phase 0 scoreboard: 8 of 10 work packages done
WP-0001..0007, 0009, 0010 done (specs, core renderer, golden harness with negative controls, sidecar v0 + model manager, demucs cartridge, CI config, stem export). WP-0008 (Basic-Pitch transcription) queued — needs a TF-on-aarch64 decision first. **Total: 43 C++ assertions + 66 Python tests green on aarch64.**


## Process notes (honest)
- The sidecar subagent died mid-WP-0005 with core green; I took over, found and fixed **one real bug it left** (`local_available()` vacuously true for zero-source models — caught by its own fixture), plus a startup race and lint/type debt. Handoffs in TASKS/ say exactly what came from where.
- Roadmap statuses updated in ROADMAP.md; Phase 0 = 7 of 8 WPs done (WP-0006 sidecar integration is next, engine-side verified).

## Decisions for you (nothing blocked)
1. **ADR approvals**: ADR-0001 GPLv3, ADR-0002 headless-first, ADR-0003 HTTP sidecar — all reversible, rationale in docs/adr/.
2. **Name** still provisional ("OpenPattern").
3. Listen-check when convenient: `tests/fixtures/goldens/demo.wav` (4s beat) and the demucs stems at `../exp-demucs/separated/htdemucs/test_input/`.
4. Optional tonight-follow-up: push repo to GitHub → CI matrix gets real validation (windows-11-arm runner label needs confirming).

## Next in queue (no input needed, will continue if you let the goal run)
- WP-0008: Basic-Pitch transcription adapter — decision first: TF-on-aarch64 install weight vs. ONNX route; hum→MIDI feature depends on it.
- Phase 1 candidates per plan: arrangement layer (clips referencing patterns) + tempo map → then the YuE2 "prompt→editable session" spike (its ABC score output maps straight into our project model).
