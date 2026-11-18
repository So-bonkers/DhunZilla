"""Conformance suite — CI runs these 5 checks for EVERY registered backend.

Implements the checklist in docs/spec/backend-adapters.md §Conformance suite:
  1. capability honesty   — declared flags match the interfaces actually implemented,
                            and every declared task actually runs (mock-level probe).
  2. determinism          — same request+seed → byte-identical artifacts
                            (only for backends declaring deterministic=True; spec scope).
  3. duration accuracy    — generated audio within ±5% of requested (generation only).
  4. graceful degradation — health never raises; tier "no" / unavailable status makes
                            the method raise BackendUnavailable with an actionable
                            message, and the runner reports a structured error.
  5. provenance           — every done job carries {backend_id, model_version, seed,
                            request_hash} in out/jobs/<job_id>/provenance.json.

Backends are discovered from the registry at collection time, so real adapters added
later (demucs etc.) are covered automatically.
"""

from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any

import pytest

from ai_sidecar.base import (
    HARDWARE_TIERS,
    BackendBase,
    BackendUnavailable,
    GenerationBackend,
    InvalidRequest,
    SeparationBackend,
    TranscriptionBackend,
    canonical_request_hash,
    supports_task,
)
from ai_sidecar.jobs import JobRecord, JobRunner
from ai_sidecar.registry import discover_backends

BACKENDS: list[BackendBase] = list(discover_backends().values())
BACKEND_IDS = [b.id for b in BACKENDS]


def backend_params(items: list[BackendBase]):
    return pytest.mark.parametrize("backend", items, ids=[b.id for b in items])


# ------------------------------------------------------------------ shared helpers


def run_job(
    backend_id: str, task: str, params: dict[str, Any], tmp_path: Path
) -> JobRecord:
    """Run one job to a terminal state through the real runner (no HTTP)."""

    async def _run() -> JobRecord:
        registry = {b.id: b for b in BACKENDS}
        runner = JobRunner(registry, jobs_dir=tmp_path / "jobs", workers=2)
        await runner.start()
        try:
            job_id = await runner.submit(backend_id, task, params)
            return await runner.wait(job_id, timeout_s=120.0)
        finally:
            await runner.stop()

    return asyncio.run(_run())


def generate_params(duration_s: float = 2.0, seed: int = 123) -> dict[str, Any]:
    return {"prompt": "conformance loop", "duration_s": duration_s, "seed": seed}


def separate_params(input_wav: Path) -> dict[str, Any]:
    return {"audio_path": str(input_wav)}


def transcribe_params(input_wav: Path) -> dict[str, Any]:
    return {"audio_path": str(input_wav)}


# ------------------------------------------------------------------ check 1


@backend_params(BACKENDS)
def test_capability_honesty(backend: BackendBase, tmp_path: Path, make_input_wav) -> None:
    """Declared flags match implemented interfaces, and declared tasks actually run."""
    caps = backend.capabilities()
    assert set(caps.vram_tiers) == set(HARDWARE_TIERS)
    assert caps.max_duration_s > 0

    structural = {
        "generate": isinstance(backend, GenerationBackend),
        "separate": isinstance(backend, SeparationBackend),
        "transcribe": isinstance(backend, TranscriptionBackend),
    }
    for task, implemented in structural.items():
        declared = supports_task(caps, task)
        assert declared == implemented, (
            f"{backend.id}: capability honesty violated for '{task}' — declares "
            f"{declared} but the interface class is {'implemented' if implemented else 'NOT'}"
        )

    # A declared generation task must actually produce audio at mock level.
    if structural["generate"]:
        rec = run_job(backend.id, "generate", generate_params(), tmp_path)
        assert rec.state == "done", f"{backend.id}: declared generate failed: {rec.error}"
        assert "audio" in (rec.result or {}).get("artifacts", {})
    if structural["separate"]:
        rec = run_job(backend.id, "separate", separate_params(make_input_wav()), tmp_path)
        assert rec.state == "done", f"{backend.id}: declared separate failed: {rec.error}"
        artifacts = (rec.result or {}).get("artifacts", {})
        assert {"vocals", "drums", "bass", "other"} <= set(artifacts)
    if structural["transcribe"]:
        rec = run_job(backend.id, "transcribe", transcribe_params(make_input_wav()), tmp_path)
        assert rec.state == "done", f"{backend.id}: declared transcribe failed: {rec.error}"
        artifacts = (rec.result or {}).get("artifacts", {})
        assert "midi" in artifacts, f"{backend.id}: transcribe must return a 'midi' artifact"

    # An undeclared task must be refused at submit time, not silently accepted.
    for task, implemented in structural.items():
        if not implemented:
            with pytest.raises(InvalidRequest, match=task):
                run_job(backend.id, task, generate_params(), tmp_path)


# ------------------------------------------------------------------ check 2


@backend_params(BACKENDS)
def test_determinism_byte_identical(
    backend: BackendBase, tmp_path: Path, make_input_wav
) -> None:
    """Same request + seed → byte-identical artifacts (spec: deterministic=True only)."""
    if not backend.capabilities().deterministic:
        pytest.skip(f"{backend.id} declares deterministic=False — exempt per spec")

    if isinstance(backend, GenerationBackend):
        task, params = "generate", generate_params(duration_s=1.5, seed=99)
    else:
        task, params = "separate", separate_params(make_input_wav("det-input.wav"))

    rec_a = run_job(backend.id, task, params, tmp_path / "run_a")
    rec_b = run_job(backend.id, task, params, tmp_path / "run_b")
    assert rec_a.state == "done" and rec_b.state == "done", (rec_a.error, rec_b.error)
    arts_a: dict[str, str] = rec_a.result["artifacts"]  # type: ignore[index]
    arts_b: dict[str, str] = rec_b.result["artifacts"]  # type: ignore[index]
    assert arts_a.keys() == arts_b.keys()
    for name in arts_a:
        bytes_a = Path(arts_a[name]).read_bytes()
        bytes_b = Path(arts_b[name]).read_bytes()
        assert bytes_a == bytes_b, f"{backend.id}: artifact '{name}' differs between runs"
    prov_a = json.loads((rec_a.out_dir / "provenance.json").read_text())
    prov_b = json.loads((rec_b.out_dir / "provenance.json").read_text())
    assert prov_a["request_hash"] == prov_b["request_hash"]


# ------------------------------------------------------------------ check 3


@pytest.mark.parametrize("backend", [b for b in BACKENDS if isinstance(b, GenerationBackend)],
                         ids=lambda b: b.id)
@pytest.mark.parametrize("duration_s", [1.0, 3.25])
def test_duration_accuracy(backend: BackendBase, duration_s: float, tmp_path: Path) -> None:
    """Generated audio duration within ±5% of the request (generation backends)."""
    from ai_sidecar.wav import read_wav

    rec = run_job(backend.id, "generate", generate_params(duration_s=duration_s), tmp_path)
    assert rec.state == "done", rec.error
    data, sample_rate = read_wav(Path(rec.result["artifacts"]["audio"]))  # type: ignore[index]
    actual = data.shape[0] / sample_rate
    rel_err = abs(actual - duration_s) / duration_s
    assert rel_err <= 0.05, (
        f"{backend.id}: requested {duration_s}s, got {actual:.4f}s "
        f"(rel err {rel_err:.3%} > 5%)"
    )


# ------------------------------------------------------------------ check 4

from conftest import CpuTierNoGenBackend, UnavailableGenBackend  # noqa: E402

_UNAVAILABLE_STUB = UnavailableGenBackend()
_TIER_NO_STUB = CpuTierNoGenBackend()


@backend_params(BACKENDS)
def test_health_never_raises_and_is_legal(backend: BackendBase) -> None:
    """health() is graceful and reports only legal tier values (all backends)."""
    info = backend.health()  # must not raise — the base wraps crashes anyway
    assert info.status in {"ok", "degraded", "unavailable"}
    assert set(info.vram_tiers) == set(HARDWARE_TIERS)
    for level in info.vram_tiers.values():
        assert level in {"ok", "tight", "no"}


def test_unavailable_backend_raises_actionable_error(tmp_path: Path) -> None:
    """health tier 'no' → BackendUnavailable with actionable message, no traceback."""
    stub = _UNAVAILABLE_STUB
    info = stub.health()
    assert info.status == "unavailable"
    assert all(level == "no" for level in info.vram_tiers.values())

    from ai_sidecar.base import GenerateRequest

    with pytest.raises(BackendUnavailable) as excinfo:
        stub.generate(GenerateRequest(prompt="x", duration_s=1.0, seed=1))
    message = str(excinfo.value)
    assert "unavailgen" in message and "missing" in message.lower()
    assert "Traceback" not in message

    registry = {stub.id: stub}

    async def _run():
        runner = JobRunner(registry, jobs_dir=tmp_path / "jobs", workers=1)
        await runner.start()
        try:
            jid = await runner.submit(stub.id, "generate", generate_params())
            rec = await runner.wait(jid, timeout_s=30.0)
        finally:
            await runner.stop()
        assert rec.state == "failed"
        assert rec.error is not None
        assert rec.error["code"] == "backend_unavailable"
        assert rec.error["retryable"] is True
        assert "Traceback" not in rec.error["message"]

    asyncio.run(_run())


def test_tier_no_backend_refuses_cpu(tmp_path: Path) -> None:
    """Backend healthy but tier cpu='no' → BackendUnavailable naming the tier."""
    from ai_sidecar.base import GenerateRequest

    stub = _TIER_NO_STUB
    assert stub.health().status == "degraded"  # base coerces any "no" tier
    with pytest.raises(BackendUnavailable, match="cpu"):
        stub.generate(GenerateRequest(prompt="x", duration_s=1.0, seed=1))


# ------------------------------------------------------------------ check 5


@backend_params(BACKENDS)
def test_provenance_present_and_correct(
    backend: BackendBase, tmp_path: Path, make_input_wav
) -> None:
    """Done jobs carry {backend_id, model_version, seed, request_hash} provenance."""
    if isinstance(backend, GenerationBackend):
        task, params = "generate", generate_params(duration_s=1.0, seed=2024)
    elif isinstance(backend, TranscriptionBackend):
        task, params = "transcribe", transcribe_params(make_input_wav("prov-input.wav"))
    else:
        task, params = "separate", separate_params(make_input_wav("prov-input.wav"))

    rec = run_job(backend.id, task, params, tmp_path)
    assert rec.state == "done", rec.error
    prov_file = rec.out_dir / "provenance.json"
    assert prov_file.is_file(), f"{backend.id}: missing provenance.json in {rec.out_dir}"

    prov = json.loads(prov_file.read_text(encoding="utf-8"))
    for key in ("backend_id", "model_version", "seed", "request_hash"):
        assert key in prov, f"{backend.id}: provenance.json lacks '{key}'"
    assert prov["backend_id"] == backend.id
    assert prov["model_version"] == backend.model_version
    expected_hash = canonical_request_hash(backend.id, task, params)
    assert prov["request_hash"] == expected_hash
    if task == "generate":
        assert prov["seed"] == 2024
