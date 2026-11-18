"""WP-0006 tests for the demucs cartridge: opt-in guard, graceful degradation,
and a real end-to-end separation through the job runner when OP_DEMUCS_PYTHON works.

Run real parts on the reference rig:
    OP_DEMUCS_PYTHON=/path/to/demucs-venv/bin/python uv run pytest tests/test_demucs_adapter.py -v
Without the env var, the real tests skip and the guard/degradation tests still pass.
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from ai_sidecar.jobs import JobRecord, JobRunner

BACKEND_DIR = Path(__file__).resolve().parent.parent / "backends"
IMPL_PATH = BACKEND_DIR / "demucs" / "impl.py"
ENV_VAR = "OP_DEMUCS_PYTHON"

_INTERP = os.environ.get(ENV_VAR, "").strip()
_HAS_REAL = False
if _INTERP:
    _probe = subprocess.run(
        [_INTERP, "-c", "import demucs"], capture_output=True, timeout=120.0
    )
    _HAS_REAL = _probe.returncode == 0

requires_real = pytest.mark.skipif(
    not _HAS_REAL, reason=f"{ENV_VAR} unset or demucs not importable there"
)


def _load_impl(fresh_name: str):
    """Fresh-import backends/demucs/impl.py (guard logic runs at import time)."""
    spec = importlib.util.spec_from_file_location(fresh_name, IMPL_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _run_job(backend_id: str, params: dict[str, object], tmp_path: Path) -> JobRecord:
    async def _run() -> JobRecord:
        backends_dir = tmp_path / "backends"
        backends_dir.mkdir(exist_ok=True)
        (backends_dir / "__init__.py").write_text("", encoding="utf-8")
        import shutil

        shutil.copytree(BACKEND_DIR / "demucs", backends_dir / "demucs")
        from ai_sidecar.registry import discover_backends

        registry = discover_backends(backends_dir)
        runner = JobRunner(registry, jobs_dir=tmp_path / "jobs", workers=1)
        await runner.start()
        try:
            job_id = await runner.submit(backend_id, "separate", params)
            return await runner.wait(job_id, timeout_s=600.0)
        finally:
            await runner.stop()

    return asyncio.run(_run())


# ------------------------------------------------------------------ guard + degradation


def test_module_refuses_import_without_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv(ENV_VAR, raising=False)
    with pytest.raises(ImportError, match=ENV_VAR):
        _load_impl("demucs_impl_guard_probe")


def test_broken_interpreter_health_is_unavailable_with_actionable_reason(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # sys.executable = sidecar venv python: exists but has no demucs.
    monkeypatch.setenv(ENV_VAR, sys.executable)
    backend = _load_impl("demucs_impl_broken_probe").BACKEND_CLASS()
    info = backend.health()
    assert info.status == "unavailable"
    assert info.reason is not None and "pip install demucs" in info.reason


def test_job_fails_structured_when_backend_unavailable(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, make_input_wav
) -> None:
    monkeypatch.setenv(ENV_VAR, sys.executable)
    rec = _run_job("demucs", {"audio_path": str(make_input_wav())}, tmp_path)
    assert rec.state == "failed"
    assert rec.error is not None and rec.error["code"] == "backend_unavailable"


# ------------------------------------------------------------------ real end-to-end


@requires_real
def test_real_separation_end_to_end(tmp_path: Path, make_input_wav) -> None:
    from ai_sidecar.wav import read_wav  # stdlib wave can't read float32 WAVs

    src = make_input_wav()
    x_in, sr_in = read_wav(src)
    in_dur = x_in.shape[0] / sr_in

    rec = _run_job("demucs", {"audio_path": str(src)}, tmp_path)
    assert rec.state == "done", f"separation failed: {rec.error}"
    result = rec.result or {}
    artifacts = result["artifacts"]
    assert set(artifacts) == {"vocals", "drums", "bass", "other"}
    for stem, path in artifacts.items():
        p = Path(path)
        assert p.is_file(), f"missing artifact {stem}: {path}"
        x, sr = read_wav(p)
        dur = x.shape[0] / sr
        assert abs(dur - in_dur) / in_dur < 0.05, f"{stem} duration off: {dur}s"
    prov_file = result["provenance_file"]
    prov = json.loads(Path(prov_file).read_text(encoding="utf-8"))
    assert prov["backend_id"] == "demucs"
    assert len(prov["request_hash"]) == 64
