---
id: WP-0151
title: Sidecar job cancellation made exact (cooperative cancel, `cancelling` state) + `GET /jobs` listing
phase: 3
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context — why this exists
`POST /jobs/{id}/cancel` already exists but is vague: a running job's cancel is "best effort", the state
stays `running` while it winds down, cancelled jobs leave partial files on disk, and the mock backends
cannot be cancelled mid-job so tests need sleeps. There is also no way to list jobs. This package pins
the semantics down (new `cancelling` state, cooperative cancel checkpoints, artifact cleanup) and adds
`GET /jobs`. Python only — no C++ changes.

## Spec — normative
The full text is in `docs/spec/sidecar-protocol.md`, section **"Job cancellation and listing — additive
(WP-0151)"** (the runner already installed it; read it first and follow it exactly). Summary of what you
implement, with the exact names:

1. `ai-sidecar/src/ai_sidecar/base.py`
   - `class JobCancelled(Exception)` (plain `Exception` subclass, docstring only).
   - `JobContext` gains field `is_cancelled: Callable[[], bool] = field(default=lambda: False)`.
   - `BackendBase.check_cancelled(self) -> None`: `if self.job_context().is_cancelled(): raise JobCancelled("job cancelled")`.
2. `ai-sidecar/src/ai_sidecar/jobs.py`
   - `JobState` literal gains `"cancelling"`; `TERMINAL_STATES` unchanged.
   - `JobRunner.cancel(job_id) -> bool`: unknown id → `KeyError` (unchanged); `cancelled` → `True`; `done`/`failed` →
     `False`; `queued` → mark cancel requested and finish as cancelled immediately; `running` → state
     `"cancelling"`, return `True`; `cancelling` → `True` (no change).
   - Every path that ends a job as cancelled goes through ONE helper (e.g. `_finish_cancelled(record)`):
     sets `state="cancelled"`, `progress=None`, `result=None`, `error=None`, and
     `shutil.rmtree(record.out_dir, ignore_errors=True)`.
   - `_execute`: pass `is_cancelled=lambda: record.cancel_requested` into `JobContext`; catch `JobCancelled`
     (before the generic `except Exception`) → cancelled; a backend that returns normally or raises any
     error while `cancel_requested` → cancelled (never `done`/`failed`).
   - `JobRunner.list_jobs(state: str | None = None) -> list[JobRecord]`: submission order (dict insertion
     order), optionally filtered by exact state.
3. `ai-sidecar/src/ai_sidecar/app.py`: `GET /jobs` → `{"jobs": [record.snapshot(), ...]}`; `?state=<s>` filter;
   state not in `queued|running|cancelling|done|failed|cancelled` → `ApiError("bad_request", msg, 422, False)`.
   The cancel route keeps its JSON shape `{"job_id", "accepted", "state"}`; unknown id stays 404 `unknown_job`.
   Register `GET /jobs` so it does not shadow `GET /jobs/{job_id}`.
4. Mock backends (`ai-sidecar/backends/mockgen|mocksep|mocktrans/impl.py`): call `self.check_cancelled()`
   right after each existing `self.report_progress(...)` call after the first (and at least once before
   any file is written). mockgen additionally implements the **`"mock:hold"` test hook** from the spec:
   if `"mock:hold"` is in `req.style_tags`, before generating it writes `partial.bin` (any bytes) into
   `ctx.output_dir`, then loops at most 3000 times: `self.check_cancelled()` then wait 10 ms with
   `threading.Event().wait(0.01)`; afterwards continues normally. Do not change any other mockgen behavior
   (output of requests without the tag must stay byte-identical).
5. `harness/job_client.py`: add `cancel(base: str, job_id: str) -> dict` (POST `/jobs/{id}/cancel`, returns the
   JSON body) and `list_jobs(base: str, state: str | None = None) -> list[dict]` (GET `/jobs` with
   `?state=` when given; returns the `"jobs"` list). Use the existing `req()`; change nothing else there.
6. Tests: new file `ai-sidecar/tests/test_job_cancel.py` with at least 6 tests using `fastapi.testclient`
   (see `tests/test_app_jobs.py`; poll with a deadline, no bare `time.sleep` assertions): queued cancel,
   running cancel via the `mock:hold` job (state `cancelling` then `cancelled`, job dir gone, no `result`
   key), cancel on done (`accepted` false, provenance still on disk), unknown id 404, `GET /jobs` order +
   filter + bad filter 422, and a non-cooperative backend (`SlowGenBackend` from `tests/conftest.py`)
   cancelled while running still ends `cancelled` with its directory removed.

## Acceptance criteria
- [x] Behavior exactly as in the spec section (state machine table, HTTP codes, bodies, cleanup, listing).
- [x] `cd ai-sidecar && uv run pytest -q` green (all existing tests plus ≥ 6 new). Run uv with
      `export UV_CACHE_DIR=<workspace>/.cache/uv`.
- [x] `uv run ruff check src backends` and `uv run mypy src` (in `ai-sidecar/`) clean.
- [x] Existing `test_cancel_queued_and_running` still passes unmodified.
- [x] No C++ changes: `git diff --quiet main -- src tests CMakeLists.txt` is clean. (The standard C++ gates
      — build, `op_tests`, goldens, `test_cli_errors.sh`, `test_glue_scripts.sh` — are therefore unaffected;
      run `harness/test_glue_scripts.sh` only if you have time.)

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), `docs/adr/`, anything under `src/`, `tests/`
  (top-level C++ tests), `CMakeLists.txt`, golden fixtures.
- `docs/spec/sidecar-protocol.md` is already updated — do not edit it.
- Real backends (`demucs`, `basicpitch`) — they stay non-cooperative; SSE format; the `opsidecar` CLI
  (no `jobs cancel`/`jobs list` subcommands now); cancelling queued jobs on shutdown.

## Git
- Branch `wp-0151-sidecar-job-cancel` from `main`. Commit: `WP-0151: sidecar job cancellation (cooperative, cancelling state) + GET /jobs`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `base.py`: new `JobCancelled(Exception)`; `JobContext.is_cancelled: Callable[[], bool]` field (default
    `lambda: False`); `BackendBase.check_cancelled()` raising it when the active job was cancelled.
  - `jobs.py`: `JobState` gains `"cancelling"` (`TERMINAL_STATES` unchanged; added `JOB_STATES` derived from
    the literal for the app's `?state=` validation). Single `_finish_cancelled(record)` helper: state
    `cancelled`, progress/result/error cleared, job dir `shutil.rmtree(..., ignore_errors=True)`.
    `cancel()` implements the spec table (queued → cancelled now; running → `cancelling`; cancelling → no-op
    true; cancelled → true; done/failed → false; unknown → `KeyError`). `_execute` passes
    `is_cancelled=lambda: record.cancel_requested`, catches `JobCancelled` before generic `Exception`, and a
    cancel requested at any point (pre-lock, backend success, backend error) finalizes as `cancelled`, never
    `done`/`failed`. New `list_jobs(state=None)` in submission order.
  - `app.py`: new `GET /jobs` → `{"jobs": [snapshot, ...]}` with exact `?state=` filter; unknown state →
    422 `bad_request`. Registered before `GET /jobs/{job_id}` (exact path can't shadow it either way).
    Cancel route unchanged — already reads `record.state` after `await runner.cancel(...)`, so queued answers
    `"cancelled"` and running answers `"cancelling"`.
  - Mocks: `check_cancelled()` added after every post-first `report_progress` (mockgen 0.3/0.8/1.0, mocksep
    0.3 + per-stem, mocktrans 0.3/1.0; all before the first file write). mockgen `"mock:hold"` hook per spec:
    writes `partial.bin`, then ≤3000× `check_cancelled()` + `threading.Event().wait(0.01)`. Untagged mockgen
    output untouched (no rng/timing change — determinism tests still pass).
  - `harness/job_client.py`: added `cancel(base, job_id)` and `list_jobs(base, state=None)` on top of `req()`.
  - New `ai-sidecar/tests/test_job_cancel.py` (7 tests): queued cancel (immediate `cancelled`, snapshot
    progress null + no result/error keys, dir removed, still listed); running cooperative cancel via
    `mock:hold` (`cancelling` → `cancelled`, `partial.bin` gone with the dir, idempotent re-cancel);
    non-cooperative `SlowGenBackend` running cancel ends `cancelled` + dir removed; done-job cancel refused
    (`accepted false`, provenance kept); unknown id 404 `unknown_job`; `GET /jobs` empty/order-equals-
    submission/snapshot-equality/`?state=done|queued`; bad filter 422 `bad_request`.
- **How verified:** (pytest summary line, ruff/mypy output)
  - `uv run pytest -q` → `109 passed, 2 skipped, 71 warnings in 14.77s` (the 2 skips and discovery
    RuntimeWarnings are pre-existing optional-backend notes). New file alone:
    `uv run pytest -q tests/test_job_cancel.py` → `7 passed, 15 warnings in 3.35s`.
  - `uv run ruff check src backends` → `All checks passed!` (also ran over `tests` → clean).
  - `uv run mypy src` → `Success: no issues found in 11 source files`.
  - `git diff --quiet main -- src tests CMakeLists.txt` → exit 0 (no C++ changes).
  - Existing `test_cancel_queued_and_running` passes unmodified (part of the 109 above).
- **PROPOSE / BLOCKED:** none. (Not pushed/merged, per package instructions.)
