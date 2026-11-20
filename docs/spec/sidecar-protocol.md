# Spec: Sidecar Protocol (app ↔ ai-sidecar)

v0 transport: **FastAPI + HTTP/JSON** (gRPC deferred — ADR-0003). The sidecar is a local process,
localhost-bound by default; the C++ app talks to it exactly like any future remote service would.

## Endpoints
```
GET  /health                      → {ok, backends: [{id, task, capabilities, health}]}
POST /jobs                        → enqueue; body {backend, task, params{...}} → {job_id}
GET  /jobs/{job_id}               → {state: queued|running|done|failed|cancelled, progress: 0..1, result?, error?}
GET  /jobs/{job_id}/events        → SSE stream of progress events (cheap polling fallback allowed)
POST /jobs/{job_id}/cancel
GET  /models                      → manifest view with local availability + license gates
POST /models/{id}/download        → fetch+verify per manifest; respects license_tier accept flag in body
```

## Job semantics
- One worker pool (CPU-bound, N=cores/2 default); one GPU lock (single inference at a time) until profiling says otherwise.
- `params` mirror backend signatures (`separate`, `generate`, `transcribe`). All artifacts land in
  `out/jobs/<job_id>/`; `provenance.json` per spec/backend-adapters.md is mandatory for done jobs.
- Errors: structured `{code, message, retryable}`; codes include `backend_unavailable`, `model_not_downloaded`, `license_not_accepted`, `inference_failed`.
- Progress: backends report 0..1 when possible; otherwise indeterminate (progress=None).

## Lifecycle
The app spawns the sidecar (`uv run opsidecar serve --port 0`), reads the port from stdout line
`SIDECAR_PORT=<n>`, health-checks with backoff, and terminates it on exit. Orphaned sidecars die
with their parent (process-group kill) — no zombie daemons.

## Job cancellation and listing — additive (WP-0151)
This section makes the existing `POST /jobs/{job_id}/cancel` exact and adds `GET /jobs`.

### States
`state` is one of `queued | running | cancelling | done | failed | cancelled`. `cancelling` is new:
cancel was requested for a job whose backend call is still in flight. Terminal states are `done`,
`failed`, `cancelled`; a terminal job never changes state again.

```
queued    --cancel--> cancelled                     (immediate, inside the cancel call)
running   --cancel--> cancelling --> cancelled      (when the backend call returns or raises JobCancelled)
cancelling--cancel--> cancelling                    (idempotent)
cancelled --cancel--> cancelled                     (idempotent)
done|failed --cancel--> unchanged
```
A job in `cancelling` ends `cancelled` even if the backend finishes successfully (non-cooperative
backends cannot be interrupted) or raises an error; it never ends `done` or `failed`.

### POST /jobs/{job_id}/cancel
Always HTTP 200 for a known id; body `{"job_id": str, "accepted": bool, "state": str}` where `state` is the
state after the call. `accepted` is true iff the job is now `cancelled` or `cancelling`.

| state before | HTTP | accepted | state in response |
|---|---|---|---|
| queued | 200 | true | `cancelled` |
| running | 200 | true | `cancelling` |
| cancelling | 200 | true | `cancelling` |
| cancelled | 200 | true | `cancelled` |
| done | 200 | false | `done` |
| failed | 200 | false | `failed` |
| unknown id | 404 | — | body `{"error": {"code": "unknown_job", "message": ..., "retryable": false}}` |

### Cancelled jobs
- Snapshot (`GET /jobs/{id}`) of a `cancelled` job: `state` `"cancelled"`, `progress` `null`, no `result`
  key, no `error` key.
- No artifacts: the directory `out/jobs/<job_id>/` (including any partial output and no
  `provenance.json`) is deleted when the job becomes `cancelled`. A `done`/`failed` job's directory is
  never touched by cancel.
- A cancelled job is still listed and fetchable by id (the record stays; only the files go).

### Cooperative cancellation (backend contract)
`BackendBase.check_cancelled() -> None` raises `ai_sidecar.base.JobCancelled` if cancel was requested for
the active job; backends SHOULD call it between work chunks. `JobCancelled` is not an error: the runner
turns it into `cancelled`, never `failed`. Backends that never call it still cancel correctly, but only
once their current call returns. The mock backends (`mockgen`, `mocksep`, `mocktrans`) call it at every
progress checkpoint.

Deterministic test hook (mockgen only): a `generate` request whose `style_tags` contains the literal
`"mock:hold"` writes `partial.bin` into the job dir, then waits (polling `check_cancelled()` every 10 ms,
at most 30 s) and only continues to a normal result if it was not cancelled in that time. This gives tests
a job that stays `running` until cancelled, with no sleeps in the test.

### GET /jobs
`200 {"jobs": [<snapshot>, ...]}` — every job the sidecar knows, oldest submission first (stable order);
each element is exactly what `GET /jobs/{job_id}` returns for that job. Optional query `?state=<s>` keeps
only jobs in that state; an unknown state value is `422` with `{"error": {"code": "bad_request", ...}}`.
Empty list when there are no jobs.

## Job event log and resumable events — additive (WP-0172)
Every job keeps an append-only **event log** so a client (the app's progress bar, a reconnecting SSE
client) sees every state/progress change, not just the latest one.

### Event shape
An event is the job snapshot (exactly what `GET /jobs/{job_id}` returns at that moment: `job_id`,
`backend`, `task`, `state`, `progress`, plus `result` / `error` when the snapshot has them) with one extra
key `seq`: an integer, 1-based, contiguous, unique per job. `seq` 1 is always the `queued` event.

### When an event is appended
After every change of `state` and every change of `progress` while `running`. Nothing is appended when
the state is unchanged and the progress is equal to the last event's progress, or both are numbers closer
than `0.001` (coalescing; `progress` on the job itself is still updated). A normal job therefore logs
`queued`, `running` (progress null), `running` 0.0 … `running` 1.0, `done`. A job cancelled while queued
logs `queued`, `cancelled`; one cancelled while running logs `…, cancelling, cancelled` (each exactly
once however many times cancel is called). A failed job ends with a `failed` event carrying `error`. The
last event of a finished job is always terminal (`done`, `failed` or `cancelled`) and equals the final
snapshot plus `seq`.

### GET /jobs/{job_id}/event-log
`200 {"job_id": "<id>", "events": [<event>, ...]}` — events with `seq > after`, in order. Unknown job:
`404 unknown_job`.

### GET /jobs/{job_id}/events (SSE, replaces the poll-latest behaviour)
Streams events with `seq > after`, replaying history first, then new events as they happen. Each frame:
```
id: <seq>
event: job
data: <event JSON, sort_keys>

```
The stream ends right after the first terminal event has been sent. If the client resumes at or past the
terminal event's `seq` (including a far-too-large value) the stream ends immediately with an empty body.

### Resume cursor (both endpoints)
`after` = query `?after=<n>` if present, else the `Last-Event-ID` request header if present and
non-empty, else `0`. It must be a non-negative integer; otherwise `422` with
`{"error": {"code": "bad_request", "message": "after must be a non-negative integer, got '<raw>'", ...}}`.

### Notes
- Events live in memory with the job record (same lifetime as `GET /jobs/{id}`); there is no persistence.
- Python surface: `JobRecord.events: list[dict]`, `JobRecord.publish()` (called by the runner at each
  mutation; backends never call it).
