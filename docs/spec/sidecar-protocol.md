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
