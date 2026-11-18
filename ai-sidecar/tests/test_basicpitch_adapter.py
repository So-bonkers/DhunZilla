"""WP-0008 tests for the basicpitch cartridge: opt-in guard, graceful degradation,
real audio→MIDI end-to-end when OP_TRANSCRIBE_PYTHON works.

Reference-rig run (ONNX venv from ../exp-basicpitch):
    OP_TRANSCRIBE_PYTHON=/home/.../exp-basicpitch/.venv_onnx/bin/python \
        uv run pytest tests/test_basicpitch_adapter.py -v
"""

from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from ai_sidecar.jobs import JobRecord, JobRunner

BACKEND_DIR = Path(__file__).resolve().parent.parent / "backends"
IMPL_PATH = BACKEND_DIR / "basicpitch" / "impl.py"
ENV_VAR = "OP_TRANSCRIBE_PYTHON"

_INTERP = os.environ.get(ENV_VAR, "").strip()
_HAS_REAL = False
if _INTERP:
    _probe = subprocess.run(
        [_INTERP, "-c", "import basic_pitch, onnxruntime"], capture_output=True, timeout=180.0
    )
    _HAS_REAL = _probe.returncode == 0

requires_real = pytest.mark.skipif(
    not _HAS_REAL, reason=f"{ENV_VAR} unset or basic_pitch/onnxruntime not importable there"
)


def _load_impl(fresh_name: str):
    spec = importlib.util.spec_from_file_location(fresh_name, IMPL_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _run_job(params: dict[str, object], tmp_path: Path) -> JobRecord:
    async def _run() -> JobRecord:
        backends_dir = tmp_path / "backends"
        backends_dir.mkdir(exist_ok=True)
        (backends_dir / "__init__.py").write_text("", encoding="utf-8")
        shutil.copytree(BACKEND_DIR / "basicpitch", backends_dir / "basicpitch")
        from ai_sidecar.registry import discover_backends

        registry = discover_backends(backends_dir)
        runner = JobRunner(registry, jobs_dir=tmp_path / "jobs", workers=1)
        await runner.start()
        try:
            job_id = await runner.submit("basicpitch", "transcribe", params)
            return await runner.wait(job_id, timeout_s=900.0)
        finally:
            await runner.stop()

    return asyncio.run(_run())


# ------------------------------------------------------------------ guard + degradation


def test_module_refuses_import_without_env(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv(ENV_VAR, raising=False)
    with pytest.raises(ImportError, match=ENV_VAR):
        _load_impl("basicpitch_impl_guard_probe")


def test_broken_interpreter_health_is_unavailable_with_actionable_reason(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv(ENV_VAR, sys.executable)  # sidecar venv: no basic_pitch there
    backend = _load_impl("basicpitch_impl_broken_probe").BACKEND_CLASS()
    info = backend.health()
    assert info.status == "unavailable"
    assert info.reason is not None and "backends/basicpitch/impl.py" in info.reason


def test_job_fails_structured_when_backend_unavailable(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, make_input_wav
) -> None:
    monkeypatch.setenv(ENV_VAR, sys.executable)
    rec = _run_job({"audio_path": str(make_input_wav())}, tmp_path)
    assert rec.state == "failed"
    assert rec.error is not None and rec.error["code"] == "backend_unavailable"


# ------------------------------------------------------------------ real end-to-end


@requires_real
def test_real_transcription_end_to_end(tmp_path: Path, make_input_wav) -> None:
    src = make_input_wav(seconds=2.0)
    rec = _run_job({"audio_path": str(src)}, tmp_path)
    assert rec.state == "done", f"transcription failed: {rec.error}"
    result = rec.result or {}
    midi = Path(result["artifacts"]["midi"])
    assert midi.is_file() and midi.stat().st_size > 14
    assert midi.read_bytes()[:4] == b"MThd", "not a standard MIDI header"
    prov = json.loads(Path(result["provenance_file"]).read_text(encoding="utf-8"))
    assert prov["backend_id"] == "basicpitch"
    assert len(prov["request_hash"]) == 64
