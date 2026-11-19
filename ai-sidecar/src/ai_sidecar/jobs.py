"""Job runner: asyncio queue + worker pool, per spec/sidecar-protocol.md.

States: queued → running (→ cancelling) → done|failed|cancelled. Artifacts land in
``out/jobs/<job_id>/`` and every done job gets a mandatory ``provenance.json``; a
cancelled job's directory is removed. Concurrency model (v0): N workers pull from one
queue; a single inference lock serializes actual backend calls ("one GPU lock until
profiling says otherwise"). Backends report progress 0..1 through the JobContext;
otherwise progress stays None. Cooperative cancellation: backends may call
``check_cancelled()`` between work chunks (raises ``JobCancelled``); non-cooperative
backends still end cancelled once their in-flight call returns.

Usage example::

    from pathlib import Path
    from ai_sidecar.registry import discover_backends
    from ai_sidecar.jobs import JobRunner, JobState

    async def main() -> None:
        runner = JobRunner(discover_backends(), jobs_dir=Path("out/jobs"))
        await runner.start()
        job_id = await runner.submit(
            "mockgen", "generate", {"prompt": "lofi", "duration_s": 2.0, "seed": 7}
        )
        record = await runner.wait(job_id)
        assert record.state == "done"
        await runner.stop()
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import shutil
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Literal, get_args

from pydantic import ValidationError

from ai_sidecar.base import (
    JOB_CONTEXT,
    BackendBase,
    GenerateRequest,
    GenerationBackend,
    InvalidRequest,
    JobCancelled,
    JobContext,
    JobResult,
    SeparationBackend,
    TranscriptionBackend,
    UnknownBackend,
    canonical_request_hash,
)

logger = logging.getLogger("opsidecar.jobs")

JobState = Literal["queued", "running", "cancelling", "done", "failed", "cancelled"]
TERMINAL_STATES: frozenset[str] = frozenset({"done", "failed", "cancelled"})
JOB_STATES: frozenset[str] = frozenset(get_args(JobState))  # every valid state token


@dataclass
class JobRecord:
    """In-memory state of one job (the single source for GET /jobs/{id} and SSE)."""

    id: str
    backend: str
    task: str
    params: dict[str, Any]
    out_dir: Path
    state: JobState = "queued"
    progress: float | None = None
    result: dict[str, Any] | None = None
    error: dict[str, Any] | None = None
    cancel_requested: bool = False

    def snapshot(self) -> dict[str, Any]:
        """JSON-ready view per the GET /jobs/{job_id} contract."""
        snap: dict[str, Any] = {
            "job_id": self.id,
            "backend": self.backend,
            "task": self.task,
            "state": self.state,
            "progress": self.progress,
        }
        if self.result is not None:
            snap["result"] = self.result
        if self.error is not None:
            snap["error"] = self.error
        return snap


def _finish_cancelled(record: JobRecord) -> None:
    """End *record* as cancelled — the ONE path that does (WP-0151).

    Clears progress/result/error (so the snapshot has no ``result``/``error`` key)
    and removes the job's whole output directory, partial files included. A
    ``done``/``failed`` job's directory is never touched by cancel.
    """
    record.state = "cancelled"
    record.progress = None
    record.result = None
    record.error = None
    shutil.rmtree(record.out_dir, ignore_errors=True)


class JobRunner:
    """Asyncio job queue with a bounded worker pool and one inference lock."""

    def __init__(
        self,
        backends: dict[str, BackendBase],
        *,
        jobs_dir: Path,
        workers: int | None = None,
    ) -> None:
        self._backends = backends
        self._jobs_dir = jobs_dir
        n_cpu = os.cpu_count() or 2
        self._workers = max(1, workers if workers is not None else n_cpu // 2)
        self._queue: asyncio.Queue[str] = asyncio.Queue()
        self._inference_lock = asyncio.Lock()
        self._records: dict[str, JobRecord] = {}
        self._worker_tasks: list[asyncio.Task[None]] = []
        self._running = False

    # -- lifecycle --------------------------------------------------------------

    async def start(self) -> None:
        """Start the worker pool (idempotent)."""
        if self._running:
            return
        self._running = True
        self._worker_tasks = [
            asyncio.create_task(self._worker_loop(i), name=f"opsidecar-worker-{i}")
            for i in range(self._workers)
        ]

    async def stop(self) -> None:
        """Stop workers; running jobs are cancelled (best effort), queued ones dropped."""
        self._running = False
        for task in self._worker_tasks:
            task.cancel()
        await asyncio.gather(*self._worker_tasks, return_exceptions=True)
        self._worker_tasks = []

    # -- submission -------------------------------------------------------------

    async def submit(self, backend: str, task: str, params: dict[str, Any]) -> str:
        """Enqueue a job; returns the job_id. Raises UnknownBackend/InvalidRequest early."""
        if backend not in self._backends:
            raise UnknownBackend(f"no backend '{backend}'; known: {sorted(self._backends)}")
        inst = self._backends[backend]
        _validate_task_matches_backend(inst, task)

        job_id = uuid.uuid4().hex[:12]
        out_dir = self._jobs_dir / job_id
        out_dir.mkdir(parents=True, exist_ok=True)
        record = JobRecord(
            id=job_id, backend=backend, task=task, params=params, out_dir=out_dir
        )
        self._records[job_id] = record
        await self._queue.put(job_id)
        return job_id

    def get(self, job_id: str) -> JobRecord | None:
        """Return the live record or None for an unknown id."""
        return self._records.get(job_id)

    async def wait(self, job_id: str, timeout_s: float = 120.0, poll_s: float = 0.05) -> JobRecord:
        """Poll until the job reaches a terminal state (test/convenience helper)."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + timeout_s
        while True:
            record = self._records.get(job_id)
            if record is None:
                raise KeyError(f"unknown job {job_id}")
            if record.state in TERMINAL_STATES:
                return record
            if loop.time() > deadline:
                raise TimeoutError(f"job {job_id} not terminal within {timeout_s}s")
            await asyncio.sleep(poll_s)

    async def cancel(self, job_id: str) -> bool:
        """Request cancellation per the WP-0151 state machine; return ``accepted``.

        =======================  =========================================
        state before             effect / return
        =======================  =========================================
        queued                   finish as cancelled immediately → True
        running                  flag requested, state → cancelling → True
        cancelling               no change → True
        cancelled                no change → True
        done / failed            unchanged → False
        =======================  =========================================

        Unknown *job_id* raises :class:`KeyError`. A job in ``cancelling`` ends
        ``cancelled`` even if the backend finishes successfully or raises; it never
        ends ``done``/``failed``.
        """
        record = self._records.get(job_id)
        if record is None:
            raise KeyError(f"unknown job {job_id}")
        if record.state == "cancelled":
            return True
        if record.state in ("done", "failed"):
            return False
        record.cancel_requested = True
        if record.state == "queued":
            _finish_cancelled(record)
        elif record.state == "running":
            record.state = "cancelling"
        # "cancelling": already requested — idempotent, nothing left to change.
        return True

    def list_jobs(self, state: str | None = None) -> list[JobRecord]:
        """All known jobs in submission order (dict insertion order), oldest first.

        With *state* given, keep only jobs whose state equals it exactly.

        Usage example::

            for rec in runner.list_jobs("done"):
                print(rec.id)
        """
        records = list(self._records.values())
        if state is not None:
            records = [rec for rec in records if rec.state == state]
        return records

    # -- internals ---------------------------------------------------------------

    async def _worker_loop(self, index: int) -> None:
        while self._running:
            try:
                job_id = await self._queue.get()
            except asyncio.CancelledError:
                return
            record = self._records.get(job_id)
            if record is None or record.state in TERMINAL_STATES:
                continue  # cancelled while queued
            try:
                await self._execute(record)
            except asyncio.CancelledError:
                _finish_cancelled(record)
                return

    async def _execute(self, record: JobRecord) -> None:
        inst = self._backends[record.backend]
        request_hash = canonical_request_hash(record.backend, record.task, record.params)
        seed = params_seed(record.params)

        async with self._inference_lock:
            if record.cancel_requested:
                _finish_cancelled(record)  # cancelled while queued / waiting for the lock
                return
            record.state = "running"

            def _progress(p: float) -> None:
                self._report(record, p)

            ctx = JobContext(
                output_dir=record.out_dir,
                request_hash=request_hash,
                seed=seed,
                report_progress=_progress,
                is_cancelled=lambda: record.cancel_requested,
            )
            token = JOB_CONTEXT.set(ctx)
            try:
                job_result = await asyncio.to_thread(_dispatch, inst, record.task, record.params)
            except asyncio.CancelledError:
                _finish_cancelled(record)
                raise
            except JobCancelled:
                # Cooperative checkpoint hit (WP-0151): cancelled, never failed.
                _finish_cancelled(record)
                return
            except Exception as exc:  # noqa: BLE001 - structured error per protocol
                if record.cancel_requested:
                    _finish_cancelled(record)  # cancel wins over failure (never ends failed)
                    return
                code = str(getattr(exc, "error_code", "inference_failed"))
                retryable = bool(getattr(exc, "retryable", True))
                record.error = {"code": code, "message": str(exc), "retryable": retryable}
                logger.warning("job %s failed (%s): %s", record.id, code, exc)
                record.state = "failed"
                return
            finally:
                JOB_CONTEXT.reset(token)

        if record.cancel_requested:
            _finish_cancelled(record)  # backend succeeded but cancel arrived → cancelled
            return
        try:
            finalize_done(record, inst, job_result)
        except InvalidRequest as exc:  # backend violated the JobResult contract
            record.state = "failed"
            record.error = {
                "code": "inference_failed",
                "message": f"backend broke its contract: {exc}",
                "retryable": False,
            }

    def _report(self, record: JobRecord, value: float) -> None:
        # Called from the backend's worker thread; plain attribute write is GIL-safe.
        if record.state == "running" and not record.cancel_requested:
            record.progress = min(1.0, max(0.0, float(value)))


def params_seed(params: dict[str, Any]) -> int | None:
    """Extract the seed from job params if present (generation tasks carry one)."""
    seed = params.get("seed")
    return int(seed) if isinstance(seed, int) and not isinstance(seed, bool) else None


def _validate_task_matches_backend(inst: BackendBase, task: str) -> None:
    """A submit must address an interface family the backend actually implements."""
    required: dict[str, Any] = {
        "generate": GenerationBackend,
        "separate": SeparationBackend,
        "transcribe": TranscriptionBackend,
    }
    if task not in required:
        raise InvalidRequest(f"unknown task '{task}'; expected one of {sorted(required)}")
    if not isinstance(inst, required[task]):
        raise InvalidRequest(
            f"backend '{inst.id}' does not implement the '{task}' interface; "
            "pick a backend that does (see GET /health)"
        )


def _dispatch(inst: BackendBase, task: str, params: dict[str, Any]) -> JobResult:
    """Synchronous adapter call — runs inside asyncio.to_thread because it is CPU-bound."""
    if task == "generate":
        try:
            req = GenerateRequest.model_validate(params)
        except ValidationError as exc:
            raise InvalidRequest(f"invalid generate params: {exc}") from exc
        assert isinstance(inst, GenerationBackend)  # checked at submit
        return inst.generate(req)

    audio = params.get("audio_path")
    if not isinstance(audio, str) or not audio:
        raise InvalidRequest(f"task '{task}' requires params.audio_path (string), got {audio!r}")
    opts_raw = params.get("opts", {})
    if not isinstance(opts_raw, dict):
        raise InvalidRequest(f"params.opts must be an object, got {type(opts_raw).__name__}")
    opts: dict[str, Any] = dict(opts_raw)

    if task == "separate":
        assert isinstance(inst, SeparationBackend)  # checked at submit
        stems: list[str] | None = None
        stems_raw = params.get("stems")
        if stems_raw is not None:
            if not isinstance(stems_raw, list) or not all(isinstance(s, str) for s in stems_raw):
                raise InvalidRequest("params.stems must be a list of strings")
            stems = [str(s) for s in stems_raw]
        return inst.separate(Path(audio), stems=stems, opts=opts)

    assert isinstance(inst, TranscriptionBackend)  # checked at submit
    return inst.transcribe(Path(audio), opts=opts)


def finalize_done(record: JobRecord, inst: BackendBase, job_result: JobResult) -> None:
    """Write the mandatory provenance.json and mark the record done (contract-checked).

    Raises InvalidRequest if the backend lied about artifacts or provenance; the caller
    maps that to a failed job with code ``inference_failed``.
    """
    prov = job_result.provenance
    if prov.backend_id != inst.id:
        raise InvalidRequest(
            f"provenance backend_id '{prov.backend_id}' != registered id '{inst.id}'"
        )
    for name, path in job_result.artifacts.items():
        if not Path(path).is_file():
            raise InvalidRequest(f"backend declared artifact '{name}' at {path} but it is missing")

    prov_path = record.out_dir / "provenance.json"
    prov_path.write_text(
        json.dumps(prov.model_dump(mode="json"), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    record.result = {
        "artifacts": {name: str(path) for name, path in job_result.artifacts.items()},
        "raw_dir": str(job_result.raw_dir) if job_result.raw_dir else None,
        "provenance_file": str(prov_path),
    }
    record.state = "done"
    record.progress = 1.0
