"""FastAPI app implementing docs/spec/sidecar-protocol.md (v0 transport).

Endpoints: GET /health, POST /jobs, GET /jobs (list, oldest first), GET /jobs/{id},
POST /jobs/{id}/cancel, GET /jobs/{id}/event-log (numbered job events, WP-0172),
GET /jobs/{id}/events (SSE replay + live, resumable via Last-Event-ID / ?after=),
GET /models, POST /models/{id}/download.

Errors are always structured ``{code, message, retryable}`` under an ``error`` key.

Usage example::

    from fastapi.testclient import TestClient
    from ai_sidecar.app import create_app

    with TestClient(create_app(jobs_dir=Path("out/jobs"))) as client:
        health = client.get("/health").json()          # ok + backend list
        job = client.post("/jobs", json={
            "backend": "mockgen", "task": "generate",
            "params": {"prompt": "chill", "duration_s": 1.0, "seed": 1},
        }).json()
        state = client.get(f"/jobs/{job['job_id']}").json()
"""

from __future__ import annotations

import asyncio
import json
import logging
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse, StreamingResponse
from pydantic import BaseModel, Field

from ai_sidecar.base import (
    BackendBase,
    BackendUnavailable,
    GenerationBackend,
    InvalidRequest,
    SeparationBackend,
    TranscriptionBackend,
    UnknownBackend,
)
from ai_sidecar.jobs import JOB_STATES, TERMINAL_STATES, JobRunner
from ai_sidecar.model_manager import (
    DownloadFailed,
    HashMismatch,
    LicenseNotAccepted,
    ManifestError,
    ModelAwaitingWeights,
    ModelManager,
    ModelNotFound,
)
from ai_sidecar.registry import discover_backends

logger = logging.getLogger("opsidecar.app")


class ApiError(Exception):
    """Structured HTTP error: {"error": {"code", "message", "retryable"}}."""

    def __init__(self, code: str, message: str, status_code: int, retryable: bool) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status_code = status_code
        self.retryable = retryable


def _model_error_to_api(exc: Exception) -> ApiError:
    if isinstance(exc, ModelAwaitingWeights):
        return ApiError(exc.error_code, str(exc), 409, False)
    if isinstance(exc, LicenseNotAccepted):
        return ApiError(exc.error_code, str(exc), 403, False)
    if isinstance(exc, ModelNotFound):
        return ApiError(exc.error_code, str(exc), 404, False)
    if isinstance(exc, ManifestError):
        return ApiError(exc.error_code, str(exc), 500, False)
    if isinstance(exc, HashMismatch):
        return ApiError(exc.error_code, str(exc), 422, False)
    if isinstance(exc, DownloadFailed):
        return ApiError(exc.error_code, str(exc), 502, True)
    return ApiError("inference_failed", f"{type(exc).__name__}: {exc}", 500, False)


class JobSubmit(BaseModel):
    """POST /jobs body per docs/spec/sidecar-protocol.md."""

    backend: str = Field(min_length=1)
    task: str = Field(min_length=1)
    params: dict[str, Any] = Field(default_factory=dict)


class DownloadBody(BaseModel):
    """POST /models/{id}/download body — the license accept flag lives here."""

    accept: bool = False


def _primary_task(backend: BackendBase) -> str:
    """Single task token for /health (capabilities carry the full detail)."""
    if isinstance(backend, GenerationBackend):
        return "generate"
    if isinstance(backend, SeparationBackend):
        return "separate"
    if isinstance(backend, TranscriptionBackend):
        return "transcribe"
    return "none"


def create_app(
    *,
    backends: dict[str, BackendBase] | None = None,
    jobs_dir: Path | None = None,
    manifest_path: Path | None = None,
    workers: int | None = None,
) -> FastAPI:
    """Build the sidecar app. All arguments are injectable for tests; defaults follow
    the spec layout (registry discovery, ``out/jobs``, ``models/manifest.json``)."""
    registry = backends if backends is not None else discover_backends()
    runner = JobRunner(registry, jobs_dir=jobs_dir or Path("out/jobs"), workers=workers)
    manager = ModelManager(manifest_path)

    @asynccontextmanager
    async def lifespan(app: FastAPI) -> AsyncIterator[None]:
        await runner.start()
        try:
            yield
        finally:
            await runner.stop()

    app = FastAPI(title="DhunZilla AI sidecar", version="0.1.0", lifespan=lifespan)
    app.state.backends = registry
    app.state.runner = runner
    app.state.manager = manager

    @app.exception_handler(ApiError)
    async def _api_error_handler(_: Request, exc: ApiError) -> JSONResponse:
        return JSONResponse(
            status_code=exc.status_code,
            content={"error": {"code": exc.code, "message": exc.message,
                               "retryable": exc.retryable}},
        )

    @app.exception_handler(RequestValidationError)
    async def _validation_handler(_: Request, exc: RequestValidationError) -> JSONResponse:
        return JSONResponse(
            status_code=422,
            content={"error": {"code": "bad_request", "message": str(exc.errors()[:3]),
                               "retryable": False}},
        )

    @app.exception_handler(UnknownBackend)
    async def _unknown_backend_handler(_: Request, exc: UnknownBackend) -> JSONResponse:
        return JSONResponse(
            status_code=404,
            content={
                "error": {
                    "code": exc.error_code,
                    "message": str(exc.args[0] if exc.args else exc),
                    "retryable": False,
                }
            },
        )

    @app.exception_handler(InvalidRequest)
    async def _invalid_request_handler(_: Request, exc: InvalidRequest) -> JSONResponse:
        return JSONResponse(
            status_code=422,
            content={"error": {"code": exc.error_code, "message": str(exc),
                               "retryable": False}},
        )

    @app.exception_handler(BackendUnavailable)
    async def _unavailable_handler(_: Request, exc: BackendUnavailable) -> JSONResponse:
        return JSONResponse(
            status_code=503,
            content={"error": {"code": exc.error_code, "message": str(exc),
                               "retryable": True}},
        )

    # -- health --------------------------------------------------------------------

    @app.get("/health")
    def health() -> dict[str, Any]:
        """{ok, backends: [{id, task, capabilities, health}]} — never raises."""
        entries: list[dict[str, Any]] = []
        ok = bool(registry)
        for backend_id in sorted(registry):
            backend = registry[backend_id]
            try:
                caps = backend.capabilities()
                entry = {
                    "id": backend.id,
                    "task": _primary_task(backend),
                    "capabilities": caps.model_dump(mode="json"),
                    "health": backend.health().model_dump(mode="json"),
                }
            except Exception as exc:  # noqa: BLE001 - /health must stay up
                ok = False
                entry = {
                    "id": backend_id,
                    "task": "none",
                    "error": f"backend introspection failed: {type(exc).__name__}: {exc}",
                }
            _health = entry.get("health")
            if isinstance(_health, dict) and _health.get("status") == "unavailable":
                ok = False
            entries.append(entry)
        return {"ok": ok, "backends": entries}

    # -- jobs ----------------------------------------------------------------------

    @app.post("/jobs", status_code=202)
    async def submit_job(body: JobSubmit) -> dict[str, str]:
        try:
            job_id = await runner.submit(body.backend, body.task, body.params)
        except UnknownBackend as exc:
            msg = str(exc.args[0] if exc.args else exc)
            raise ApiError("unknown_backend", msg, 404, False) from exc
        except InvalidRequest as exc:
            raise ApiError(exc.error_code, str(exc), 422, False) from exc
        return {"job_id": job_id}

    @app.get("/jobs")
    def list_jobs(state: str | None = None) -> dict[str, Any]:
        """All job snapshots, oldest submission first; optional exact ``?state=`` filter.

        Usage example::

            done_ids = [j["job_id"] for j in client.get("/jobs?state=done").json()["jobs"]]
        """
        if state is not None and state not in JOB_STATES:
            raise ApiError(
                "bad_request",
                f"unknown state '{state}'; expected one of {sorted(JOB_STATES)}",
                422,
                False,
            )
        return {"jobs": [rec.snapshot() for rec in runner.list_jobs(state)]}

    def _require_record(job_id: str) -> Any:
        record = runner.get(job_id)
        if record is None:
            raise ApiError("unknown_job", f"no job '{job_id}' (it may have expired)", 404, False)
        return record

    def _cursor(request: Request, after: str | None) -> int:
        """Resume cursor for the event endpoints (WP-0172).

        ``?after=<n>`` wins; otherwise a present, non-empty ``Last-Event-ID`` header;
        otherwise ``0``. Anything but a non-negative integer is a 422 ``bad_request``.

        Usage example::

            cursor = _cursor(request, after)   # -> events with seq > cursor
        """
        raw = after
        if raw is None:
            header = request.headers.get("Last-Event-ID") or ""
            if not header:
                return 0
            raw = header
        message = f"after must be a non-negative integer, got '{raw}'"
        try:
            value = int(raw)
        except ValueError as exc:
            raise ApiError("bad_request", message, 422, False) from exc
        if value < 0:
            raise ApiError("bad_request", message, 422, False)
        return value

    @app.get("/jobs/{job_id}")
    def get_job(job_id: str) -> dict[str, Any]:
        snap: dict[str, Any] = _require_record(job_id).snapshot()
        return snap

    @app.post("/jobs/{job_id}/cancel")
    async def cancel_job(job_id: str) -> dict[str, Any]:
        record = _require_record(job_id)
        accepted = await runner.cancel(job_id)
        return {"job_id": job_id, "accepted": accepted, "state": record.state}

    @app.get("/jobs/{job_id}/event-log")
    def job_event_log(job_id: str, request: Request, after: str | None = None) -> dict[str, Any]:
        """Append-only per-job event log (WP-0172): events with ``seq > after``.

        Usage example::

            tail = client.get(f"/jobs/{j}/event-log", params={"after": 3}).json()["events"]
        """
        record = _require_record(job_id)
        cursor = _cursor(request, after)
        # Copy first: a worker thread may append while we filter (list copy is atomic).
        events = list(record.events)
        return {"job_id": job_id, "events": [e for e in events if int(e["seq"]) > cursor]}

    @app.get("/jobs/{job_id}/events")
    async def job_events(
        job_id: str, request: Request, after: str | None = None
    ) -> StreamingResponse:
        """SSE stream of the job event log (WP-0172): replay history, then live events.

        Each frame is ``id: <seq>`` / ``event: job`` / ``data: <event json>``; the
        stream ends right after the first terminal event, or immediately when the
        client resumes at or past it (``Last-Event-ID`` / ``?after=``).

        Usage example::

            with client.stream("GET", f"/jobs/{j}/events", params={"after": 2}) as s:
                for line in s.iter_lines():
                    ...  # "id: 3", "event: job", "data: {...}", ""
        """
        _require_record(job_id)
        cursor = _cursor(request, after)

        async def stream() -> AsyncIterator[str]:
            sent = cursor
            while True:
                record = runner.get(job_id)
                if record is None:
                    break  # job vanished mid-stream
                events = list(record.events)  # atomic copy; workers append concurrently
                for event in events:
                    seq = int(event["seq"])
                    if seq <= sent:
                        continue  # already delivered (or skipped by the cursor)
                    yield f"id: {seq}\nevent: job\ndata: {json.dumps(event, sort_keys=True)}\n\n"
                    sent = seq
                    if event.get("state") in TERMINAL_STATES:
                        return  # terminal frame delivered: stream is over
                if events and events[-1].get("state") in TERMINAL_STATES:
                    return  # resumed at/past the terminal event: nothing left, don't hang
                await asyncio.sleep(0.05)

        return StreamingResponse(
            stream(), media_type="text/event-stream", headers={"Cache-Control": "no-cache"}
        )

    # -- models --------------------------------------------------------------------

    @app.get("/models")
    def list_models() -> dict[str, Any]:
        try:
            rows = manager.view()
        except ManifestError as exc:
            raise ApiError(exc.error_code, str(exc), 503, False) from exc
        return {"manifest": str(manager.manifest_path), "models": rows}

    @app.post("/models/{model_id}/download")
    async def download_model(model_id: str, body: DownloadBody) -> dict[str, Any]:
        try:
            outcome = await asyncio.to_thread(manager.download, model_id, accept=body.accept)
        except (ModelAwaitingWeights, LicenseNotAccepted, ModelNotFound, ManifestError,
                HashMismatch, DownloadFailed) as exc:
            raise _model_error_to_api(exc) from exc
        return {"id": outcome["id"], "status": "downloaded",
                "files": outcome["files"], "sha256_verified": outcome["sha256_verified"]}

    return app
