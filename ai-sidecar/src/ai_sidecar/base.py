"""Adapter interface shared by all DhunZilla AI backends (docs/spec/backend-adapters.md).

Models are cartridges, not dependencies: the sidecar core never imports a model
library directly — it only talks to :class:`BackendBase` subclasses discovered from
the ``backends/`` directory (see :mod:`ai_sidecar.registry`).

Usage example::

    from ai_sidecar.base import Capabilities, GenerationBackend, GenerateRequest

    class MyBackend(GenerationBackend):
        id = "my-gen"
        model_version = "0.1.0"

        def capabilities(self) -> Capabilities:
            return Capabilities(text2loop=True, deterministic=True,
                                vram_tiers={"cpu": "ok", "6gb": "ok",
                                            "12gb": "ok", "24gb": "ok"},
                                max_duration_s=60.0)

        def _check_health(self) -> HealthInfo:
            return HealthInfo(status="ok", vram_tiers=self.capabilities().vram_tiers)

        def generate(self, req: GenerateRequest) -> JobResult:
            ctx = self.job_context()          # set by the job runner
            out = ctx.output_dir / "audio.wav"
            ...write out...
            return JobResult(artifacts={"audio": out},
                             provenance=self.new_provenance(seed=req.seed))
"""

from __future__ import annotations

import abc
import contextvars
import hashlib
import json
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, ClassVar, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator, model_validator

HardwareTier = Literal["cpu", "6gb", "12gb", "24gb"]
"""Hardware tiers, keyed exactly as in docs/spec/backend-adapters.md."""

VramLevel = Literal["ok", "tight", "no"]
"""Per-tier availability level. ``"no"`` means this backend cannot run there."""

HARDWARE_TIERS: tuple[HardwareTier, ...] = ("cpu", "6gb", "12gb", "24gb")
"""The canonical tier keys every Capabilities/HealthInfo must declare."""

JobTask = Literal["generate", "separate", "transcribe"]
"""Job task tokens used by POST /jobs (one per adapter interface family)."""

GENERATION_TASK_FLAGS: tuple[str, ...] = (
    "text2song",
    "text2loop",
    "repaint",
    "extend",
    "cover",
    "score2audio",
)
"""Capability flags that make a backend eligible for the ``generate`` task."""


def canonical_request_hash(backend_id: str, task: str, params: dict[str, Any]) -> str:
    """Return the sha256 hex digest of a job request, used in provenance.json.

    Canonicalization is ``json.dumps(..., sort_keys=True)`` so the hash is stable
    across runs and key insertion orders.

    Usage example::

        h = canonical_request_hash("mockgen", "generate", {"seed": 7, "duration_s": 2.0})
        assert len(h) == 64
    """
    blob = json.dumps(
        {"backend": backend_id, "task": task, "params": params},
        sort_keys=True,
        separators=(",", ":"),
    )
    return hashlib.sha256(blob.encode("utf-8")).hexdigest()


class Capabilities(BaseModel):
    """Honest capability declaration; the conformance suite verifies every flag."""

    model_config = ConfigDict(extra="forbid")

    text2song: bool = False
    text2loop: bool = False
    repaint: bool = False
    extend: bool = False
    cover: bool = False
    score2audio: bool = False
    vocal_edit: bool = False
    stem_separation: bool = False
    transcription: bool = False

    deterministic: bool = False
    """True → same request+seed must produce byte-identical artifacts
    (conformance check 2 in docs/spec/backend-adapters.md)."""

    vram_tiers: dict[HardwareTier, VramLevel]
    max_duration_s: float = Field(gt=0)

    @field_validator("vram_tiers")
    @classmethod
    def _all_tiers_declared(
        cls, v: dict[HardwareTier, VramLevel]
    ) -> dict[HardwareTier, VramLevel]:
        if set(v.keys()) != set(HARDWARE_TIERS):
            raise ValueError(f"vram_tiers must declare exactly {list(HARDWARE_TIERS)}")
        return v


class HealthInfo(BaseModel):
    """Result of ``health()`` — graceful, never raises (BackendBase wraps it)."""

    model_config = ConfigDict(extra="forbid")

    status: Literal["ok", "degraded", "unavailable"]
    vram_tiers: dict[HardwareTier, VramLevel]
    reason: str | None = None
    """Human-actionable explanation; mandatory when status == 'unavailable'."""

    @field_validator("vram_tiers")
    @classmethod
    def _all_tiers_declared(
        cls, v: dict[HardwareTier, VramLevel]
    ) -> dict[HardwareTier, VramLevel]:
        if set(v.keys()) != set(HARDWARE_TIERS):
            raise ValueError(f"vram_tiers must declare exactly {list(HARDWARE_TIERS)}")
        return v

    @model_validator(mode="after")
    def _unavailable_needs_reason(self) -> HealthInfo:
        if self.status == "unavailable" and not (self.reason and self.reason.strip()):
            raise ValueError("unavailable health must carry an actionable reason")
        return self


class GenerateRequest(BaseModel):
    """Request payload for GenerationBackend.generate (spec: backend-adapters.md)."""

    model_config = ConfigDict(extra="forbid")

    prompt: str = Field(min_length=1)
    lyrics: str | None = None
    style_tags: list[str] = Field(default_factory=list)
    duration_s: float = Field(gt=0)
    bpm: float | None = Field(default=None, gt=0)
    key: str | None = None
    seed: int
    hardware_hint: HardwareTier | None = None


class ProvenanceInfo(BaseModel):
    """Mandatory provenance block written to out/jobs/<job_id>/provenance.json."""

    model_config = ConfigDict(extra="forbid")

    backend_id: str = Field(min_length=1)
    model_version: str = Field(min_length=1)
    seed: int | None = None
    request_hash: str = Field(pattern=r"^[0-9a-f]{64}$")


class JobResult(BaseModel):
    """What every backend method returns: artifact paths + provenance."""

    model_config = ConfigDict(extra="forbid")

    artifacts: dict[str, Path] = Field(min_length=1)
    provenance: ProvenanceInfo
    """Mandatory for every JobResult (conformance check 5)."""
    raw_dir: Path | None = None
    """Optional directory with unprocessed intermediate outputs (YuE2-style plans etc.)."""


class BackendUnavailable(RuntimeError):
    """Raised when a backend cannot serve a request now; message must be actionable.

    The job runner maps this to the structured error code ``backend_unavailable``
    (retryable). Never raise raw exceptions across the adapter boundary — wrap them.
    """

    error_code = "backend_unavailable"
    retryable = True


class InvalidRequest(ValueError):
    """Raised by backends/runner for semantically bad requests (code ``bad_request``)."""

    error_code = "bad_request"
    retryable = False


class UnknownBackend(KeyError):
    """No backend with that id is registered (code ``unknown_backend``, 404-ish)."""

    error_code = "unknown_backend"
    retryable = False


class JobCancelled(Exception):
    """Raised by :meth:`BackendBase.check_cancelled` when cancel was requested.

    Not an error: the job runner turns it into a ``cancelled`` job, never ``failed``
    (docs/spec/sidecar-protocol.md, WP-0151). Backends call ``check_cancelled()``
    between work chunks; backends that never call it still cancel once their current
    call returns.
    """


def supports_task(caps: Capabilities, task: str) -> bool:
    """Return True if *caps* declares support for a job task token.

    Usage example::

        supports_task(Capabilities(stem_separation=True, ...), "separate")  # True
    """
    if task == "generate":
        return any(getattr(caps, flag) for flag in GENERATION_TASK_FLAGS)
    if task == "separate":
        return caps.stem_separation
    if task == "transcribe":
        return caps.transcription
    return False


@dataclass
class JobContext:
    """Per-job execution context the runner installs before calling a backend.

    Backends read it via :meth:`BackendBase.job_context` — the adapter signatures in
    docs/spec/backend-adapters.md have no output-dir parameter, so the runner injects
    it through this contextvar (propagated into worker threads by ``asyncio.to_thread``).
    """

    output_dir: Path
    request_hash: str
    seed: int | None = None
    report_progress: Callable[[float], None] = field(default=lambda p: None)
    is_cancelled: Callable[[], bool] = field(default=lambda: False)
    """True once cancellation was requested for this job (WP-0151); poll it via
    :meth:`BackendBase.check_cancelled` between work chunks."""


JOB_CONTEXT: contextvars.ContextVar[JobContext | None] = contextvars.ContextVar(
    "opsidecar_job_context", default=None
)


class BackendBase(abc.ABC):
    """Common base for all adapters. Subclasses set ``id``/``model_version``, declare
    :meth:`capabilities`, implement :meth:`_check_health`, and implement at least one
    of GenerationBackend / SeparationBackend / TranscriptionBackend below.

    Contract:
      - ``health()`` never raises (crashes are converted to an unavailable HealthInfo);
      - methods raise only :class:`BackendUnavailable` / :class:`InvalidRequest`;
      - artifacts are written under ``job_context().output_dir`` and returned in
        :attr:`JobResult.artifacts`; provenance is attached via :meth:`new_provenance`.
    """

    id: ClassVar[str] = ""
    model_version: ClassVar[str] = "unknown"

    @abc.abstractmethod
    def capabilities(self) -> Capabilities:
        """Honest declaration; the conformance suite probes that this matches reality."""

    def health(self) -> HealthInfo:
        """Deps present? vram tier? Graceful — never raises (spec requirement)."""
        try:
            info = self._check_health()
        except Exception as exc:  # noqa: BLE001 - contract: health must not raise
            return HealthInfo(
                status="unavailable",
                vram_tiers={tier: "no" for tier in HARDWARE_TIERS},
                reason=f"{self.id}: health check crashed: {type(exc).__name__}: {exc}",
            )
        if info.status == "ok" and any(level == "no" for level in info.vram_tiers.values()):
            # A tier reported "no" is at best degraded service, not fully healthy.
            return info.model_copy(update={"status": "degraded"})
        return info

    @abc.abstractmethod
    def _check_health(self) -> HealthInfo:
        """Implement me: inspect deps/weights/hardware and report honestly."""

    # -- job execution helpers -------------------------------------------------

    @staticmethod
    def job_context() -> JobContext:
        """Return the active JobContext; raise a clear error if outside a job.

        Usage example::

            ctx = MyBackend.job_context()
            out = ctx.output_dir / "audio.wav"
        """
        ctx = JOB_CONTEXT.get()
        if ctx is None:
            raise RuntimeError(
                "backend method called outside a job context; the sidecar job runner "
                "(ai_sidecar.jobs.JobRunner) must set JOB_CONTEXT before dispatch"
            )
        return ctx

    def new_provenance(self, seed: int | None = None) -> ProvenanceInfo:
        """Build the mandatory provenance block for the active job context."""
        ctx = self.job_context()
        return ProvenanceInfo(
            backend_id=self.id,
            model_version=self.model_version,
            seed=seed if seed is not None else ctx.seed,
            request_hash=ctx.request_hash,
        )

    def report_progress(self, value: float) -> None:
        """Report progress 0..1 for the active job (spec: indeterminate = don't call)."""
        self.job_context().report_progress(min(1.0, max(0.0, float(value))))

    def check_cancelled(self) -> None:
        """Raise :class:`JobCancelled` if cancel was requested for the active job.

        Call between work chunks; the runner maps it to a ``cancelled`` job.

        Usage example::

            for chunk in chunks:
                self.check_cancelled()
                process(chunk)
        """
        if self.job_context().is_cancelled():
            raise JobCancelled("job cancelled")

    def ensure_available(self, hardware: HardwareTier = "cpu") -> None:
        """Raise :class:`BackendUnavailable` if health says this backend can't serve now.

        Called by backends at the top of each method and by the runner before dispatch.
        """
        info = self.health()
        if info.status == "unavailable":
            raise BackendUnavailable(
                f"{self.id}: unavailable — {info.reason} (fix the cause, then retry)"
            )
        if info.vram_tiers.get(hardware) == "no":
            raise BackendUnavailable(
                f"{self.id}: cannot run on hardware tier '{hardware}' "
                f"(health reports tier 'no'; reason: {info.reason or 'none given'}). "
                "Pass a different hardware_hint or choose another backend."
            )


class GenerationBackend(BackendBase):
    """Adapter family for text→audio generation (mockgen, acestep, yue2...)."""

    @abc.abstractmethod
    def generate(self, req: GenerateRequest) -> JobResult:
        """Generate audio; write artifacts under job_context().output_dir."""


class SeparationBackend(BackendBase):
    """Adapter family for stem separation (mocksep, demucs...)."""

    DEFAULT_STEMS: ClassVar[tuple[str, ...]] = ("vocals", "drums", "bass", "other")
    """Default stem set per docs/spec/backend-adapters.md."""

    @abc.abstractmethod
    def separate(
        self,
        audio_path: Path,
        *,
        stems: list[str] | None = None,
        opts: dict[str, Any],
    ) -> JobResult:
        """Separate *audio_path* into stems; artifacts map ``{stem_name: wav_path}``.

        Outputs are 44.1 kHz stereo float32 WAVs inside the job output dir.
        """


class TranscriptionBackend(BackendBase):
    """Adapter family for audio→MIDI transcription (basic-pitch...)."""

    @abc.abstractmethod
    def transcribe(self, audio_path: Path, *, opts: dict[str, Any]) -> JobResult:
        """Transcribe *audio_path*; artifact key ``"midi"`` must be present."""
