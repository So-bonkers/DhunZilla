---
id: WP-0172
title: Sidecar job event log - every state/progress change as a numbered, resumable event
phase: 3
status: ready
depends_on: [WP-0151]
estimate_agent_hours: 1.0
human_review: none
---

## Context - why this exists
`GET /jobs/{id}/events` currently polls the job snapshot every 100 ms and emits only when it changed, so a
fast progress sequence collapses to whatever the poll happened to see, and a client that reconnects loses
history. The app's progress bar (Phase 3 stem-split UX) needs every step and a way to resume. This package
adds a per-job append-only event log with contiguous `seq` numbers, a JSON endpoint for it, and an SSE stream
that replays and resumes (`Last-Event-ID`). The normative text is the new section "Job event log and
resumable events (WP-0172)" at the end of `docs/spec/sidecar-protocol.md` (already updated in the repo; read
it, do not edit it).

## Spec - exact interface
All paths below are under `ai-sidecar/`.

`src/ai_sidecar/jobs.py`
- `JobRecord` gains `events: list[dict[str, Any]]` (default empty list), a private
  `_events_lock: threading.Lock` (dataclass field, `repr=False`), and
  `def publish(self) -> None`: under the lock, take `self.snapshot()`; if there is a last event and its
  `state` equals the snapshot's `state` and (its `progress` equals the snapshot's, or both are not None and
  differ by less than `0.001`), return without appending; otherwise set `snap["seq"] = len(self.events) + 1`
  and append it.
- Call `record.publish()` immediately after EVERY mutation of `state` or `progress`: in `submit` (after the
  record is stored, before it is queued), in `cancel` (running -> cancelling), in `_finish_cancelled` (last
  line), in `_execute` (state -> running; the failed branch), in `JobRunner._report` (after progress is
  stored), in the `finalize_done` function (last line, after state -> done) and where `_execute` marks a
  contract-breaking job failed. Backends never call `publish`.
- Do not change `snapshot()` or any existing endpoint's body.

`src/ai_sidecar/app.py`
- A helper `_cursor(request, after)` inside `create_app`: `after` (the `?after=` query string) if given,
  else the `Last-Event-ID` header if present and non-empty, else `0`. Parse with `int`; non-integer or
  negative -> `ApiError("bad_request", f"after must be a non-negative integer, got '{raw}'", 422, False)`.
- New `GET /jobs/{job_id}/event-log` (query `after: str | None = None`): unknown job -> the existing
  `unknown_job` 404; returns `{"job_id": job_id, "events": [events with seq > cursor]}`.
- `GET /jobs/{job_id}/events` is rewritten: same media type and `Cache-Control: no-cache`; same cursor
  parsing (bad cursor -> 422 JSON error BEFORE streaming starts); streams every event with `seq > cursor`
  in order as the frame `id: <seq>\nevent: job\ndata: <json.dumps(event, sort_keys=True)>\n\n`; stops right
  after sending a terminal-state event; also stops immediately if the log's last event is terminal and its
  `seq <= ` the highest seq already sent/skipped (resume past the end must not hang). Between checks
  `await asyncio.sleep(0.05)`.

`tests/test_job_events.py` (new): at least 6 tests using `TestClient(create_app(jobs_dir=tmp_path/"jobs",
workers=1))` and the mock backends only (no network, no GPU): normal job event sequence (first `queued`,
last `done` equal to the final snapshot plus `seq`, `seq` contiguous, progress non-decreasing); `event-log`
`?after=`; `Last-Event-ID` resume on the log endpoint; bad cursor 422; SSE full replay of a finished job
(`id:` lines equal the seqs) and resume past the end terminates; queued-cancel logs `queued, cancelled`
(use `style_tags: ["mock:hold"]` on a first job with `workers=1` so the second stays queued - see
`tests/test_job_cancel.py`).

## Acceptance criteria
- [x] Everything above implemented; all existing sidecar tests still pass unchanged
      (`cd ai-sidecar && UV_CACHE_DIR=<workspace>/.cache/uv uv run pytest -q`) and `uv run ruff check .` is clean.
- [x] For a finished mockgen job the event log is exactly: `queued`, `running`(null), `running` 0.0, 0.3, 0.8,
      1.0, `done` (the check reads this).
- [x] New tests as above.

## Out of scope - do NOT touch
- `ROADMAP.md`, `src/` (C++), `tests/` (repo root), `harness/`, `.github/`, `docs/` (spec already updated),
  `ai-sidecar/backends/`, `ai-sidecar/src/ai_sidecar/base.py`, `model_manager.py`, `cli.py`.
- No event persistence, no WebSocket, no backend API change, no change to the snapshot shape.

## Git
- Branch `wp-0172-job-event-log` from `main`. Commit: `WP-0172: sidecar job event log + resumable SSE`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - `ai-sidecar/src/ai_sidecar/jobs.py` — `JobRecord` gains `events: list[dict[str, Any]]` (default empty),
    a private `_events_lock: threading.Lock` (`field(repr=False)`) and `publish()`: under the lock it takes
    `snapshot()` and appends it with `seq = len(events) + 1`, unless the last event has the same `state` and
    equal progress (or both non-None and `< 0.001` apart → coalesced). `publish()` is now called immediately
    after every `state`/`progress` mutation: `submit` (after storing the record, before queueing), `cancel`
    (running → cancelling), `_finish_cancelled` (last line, covers every cancel path incl. cooperative
    `JobCancelled`, worker teardown and cancel-beats-failure), `_execute` (→ running; the failed branch; the
    contract-breaking `InvalidRequest` branch), `JobRunner._report` (after progress is stored) and
    `finalize_done` (last line, after → done). `snapshot()` unchanged; backends never call `publish`.
  - `ai-sidecar/src/ai_sidecar/app.py` — new `_cursor(request, after)` helper (`?after=` wins, else a present
    non-empty `Last-Event-ID`, else `0`; anything but a non-negative integer → `ApiError("bad_request",
    "after must be a non-negative integer, got '<raw>'", 422, False)`); new
    `GET /jobs/{job_id}/event-log?after=` returning `{"job_id", "events": [seq > cursor]}` (unknown job → the
    existing `unknown_job` 404); `GET /jobs/{job_id}/events` rewritten to replay-then-live stream of the log —
    frames `id: <seq>\nevent: job\ndata: <json.dumps(event, sort_keys=True)>\n\n`, same media type and
    `Cache-Control: no-cache`, cursor validated as JSON **before** streaming starts, ends right after a terminal
    event and immediately when resuming at/past it (no hang), `await asyncio.sleep(0.05)` between checks.
  - `ai-sidecar/tests/test_job_events.py` — new, 10 tests, mock backends only (`workers=1`, no network/GPU):
    exact finished-mockgen sequence (+ contiguity, non-decreasing progress, last event == final snapshot + seq),
    `event-log?after=`, `Last-Event-ID` resume on the log (and query-wins precedence), bad cursor 422 JSON on
    both endpoints (`abc`, `-1`, `1.5`, empty, and a bogus header), unknown job 404, SSE full replay matching
    the log frame-for-frame, SSE resume mid-log + resume at/past the end terminates with an empty body (run in
    a helper thread with a timeout so a hang fails loudly instead of blocking), SSE live progress stream,
    queued-cancel logs exactly `queued, cancelled` (first job parked with `style_tags:["mock:hold"]`, dir gone),
    and running-cancel logs exactly one `cancelling` + one `cancelled` despite three cancel calls.
  - `docs/spec/sidecar-protocol.md` — included **unchanged** (the normative section was pre-installed by the
    runner; not edited).
- **How verified:** all commands run on this aarch64 rig with `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`.

  1. Full sidecar suite (existing tests unchanged, plus the new file) — `cd ai-sidecar && uv run pytest -q`:
     ```
     119 passed, 2 skipped, 91 warnings in 15.26s
     ```
     (baseline before any edit on this branch: `109 passed, 2 skipped, 71 warnings in 14.95s` — the +10 are the
     new tests; nothing existing changed or was disabled.)
  2. New file alone — `uv run pytest tests/test_job_events.py -q`:
     ```
     10 passed, 21 warnings in 0.74s
     ```
  3. Lint — `uv run ruff check .`:
     ```
     All checks passed!
     ```
     mypy (repo convention, not in CI) on the two changed modules — `uv run mypy src/ai_sidecar/jobs.py src/ai_sidecar/app.py`:
     ```
     Success: no issues found in 2 source files
     ```
  4. AC#2 evidence — standalone run (`uv run python` against `TestClient(create_app(jobs_dir=tmp, workers=1))`,
     mockgen job `duration_s=0.5, seed=7`):
     ```
     final state: done
     event log  : [('queued', None), ('running', None), ('running', 0.0), ('running', 0.3), ('running', 0.8), ('running', 1.0), ('done', 1.0)]
     seqs       : [1, 2, 3, 4, 5, 6, 7]
     last == final snapshot + seq: True | terminal seq: 7
     resume after=5 -> frames: 2
     ---
     id: 6
     event: job
     data: {"backend": "mockgen", "job_id": "929747c12c12", "progress": 1.0, "seq": 6, "state": "running", "task": "generate"}
     ---
     id: 7
     event: job
     data: {"backend": "mockgen", "job_id": "929747c12c12", "progress": 1.0, "result": {...}, "seq": 7, "state": "done", "task": "generate"}
     resume past end (after=9999) body repr: ''
     bad cursor: 422 {'error': {'code': 'bad_request', 'message': "after must be a non-negative integer, got '-3'", 'retryable': False}}
     ```
  5. Cross-module smoke (no consumer of `/events` in `harness/`, checked `harness/job_client.py`: no SSE usage) —
     `bash harness/test_glue_scripts.sh`:
     ```
     test_glue_scripts: 0 failures        (21 PASS lines, exit 0; opd unchanged)
     ```
- **Metrics:** +39/-14 lines in `jobs.py`, +85/-14 in `app.py`, +306-line test file (10 tests); event log for a
  finished mockgen job = 7 events, seqs 1–7 contiguous; SSE poll interval 50 ms (was 100 ms) with no snapshot
  collapsing. No C++/CLI/golden touched → renders unaffected, so no demo WAV attached.
- **PROPOSE / BLOCKED:** nothing blocked.
  - PROPOSE: `docs/spec/sidecar-protocol.md` and this package leave the precedence of *unknown job* vs
    *bad cursor* undefined when both apply (both endpoints here answer 404 first, matching every other
    `/jobs/{id}…` route). Worth one clarifying sentence in the spec next time it is touched.
  - PROPOSE: events are in-memory only and grow with progress-report volume; a future stem-split UX that reports
    at high frequency may want a per-job cap or ring buffer — out of scope here ("no event persistence").

