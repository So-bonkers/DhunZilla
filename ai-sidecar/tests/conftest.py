"""Shared fixtures: stub backends, a local Range-capable file server, WAV helpers.

The file server exists so WP-0005 downloader tests never touch the external network;
it is bound to 127.0.0.1 and serves one temp directory with HTTP Range support.
"""

from __future__ import annotations

import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

import numpy as np
import pytest

from ai_sidecar.base import (
    Capabilities,
    GenerateRequest,
    GenerationBackend,
    HealthInfo,
    JobResult,
)
from ai_sidecar.wav import write_wav

ALL_OK_TIERS = {"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"}


class SlowGenBackend(GenerationBackend):
    """Generation backend that takes ~2 s per job — used to exercise cancellation."""

    id = "slowgen"
    model_version = "slow-0.1.0"

    def __init__(self, sleep_s: float = 2.0) -> None:
        self._sleep_s = sleep_s

    def capabilities(self) -> Capabilities:
        return Capabilities(
            text2loop=True, deterministic=False, vram_tiers=dict(ALL_OK_TIERS), max_duration_s=60.0
        )

    def _check_health(self) -> HealthInfo:
        return HealthInfo(status="ok", vram_tiers=dict(ALL_OK_TIERS))

    def generate(self, req: GenerateRequest) -> JobResult:
        self.ensure_available(req.hardware_hint or "cpu")
        ctx = self.job_context()
        time.sleep(self._sleep_s)
        out = ctx.output_dir / "audio.wav"
        write_wav(out, np.zeros((4410, 2), dtype=np.float32), 44100)
        return JobResult(artifacts={"audio": out}, provenance=self.new_provenance(req.seed))


class UnavailableGenBackend(GenerationBackend):
    """Health tier 'no' + unavailable status — conformance check-4 fixture backend."""

    id = "unavailgen"
    model_version = "0.0.0"

    def capabilities(self) -> Capabilities:
        return Capabilities(
            text2loop=True, deterministic=False, vram_tiers=dict(ALL_OK_TIERS), max_duration_s=60.0
        )

    def _check_health(self) -> HealthInfo:
        return HealthInfo(
            status="unavailable",
            vram_tiers={t: "no" for t in ALL_OK_TIERS},
            reason="weights file models/cache/fake/model.bin missing — run the model download",
        )

    def generate(self, req: GenerateRequest) -> JobResult:  # pragma: no cover - guarded
        self.ensure_available("cpu")
        raise AssertionError("unreachable: ensure_available must raise first")


class CpuTierNoGenBackend(GenerationBackend):
    """Healthy backend that cannot run on the 'cpu' tier (GPU-only claim)."""

    id = "gpugen"
    model_version = "0.0.0"

    def capabilities(self) -> Capabilities:
        tiers = dict(ALL_OK_TIERS)
        tiers["cpu"] = "no"
        return Capabilities(
            text2loop=True, deterministic=False, vram_tiers=tiers, max_duration_s=60.0
        )

    def _check_health(self) -> HealthInfo:
        tiers = dict(ALL_OK_TIERS)
        tiers["cpu"] = "no"
        return HealthInfo(status="ok", vram_tiers=tiers, reason="CUDA device required")

    def generate(self, req: GenerateRequest) -> JobResult:  # pragma: no cover - guarded
        self.ensure_available(req.hardware_hint or "cpu")
        raise AssertionError("unreachable on cpu tier")


# ------------------------------------------------------------------ local file server


class _RangeHandler(BaseHTTPRequestHandler):
    """GET with optional Range header; records every request for assertions."""

    server_version = "opsidecar-testfileserver/1.0"
    served_dir: Path = Path(".")
    requests_log: list[dict[str, Any]] = []

    def do_GET(self) -> None:  # noqa: N802 - stdlib naming
        path = (self.served_dir / self.path.lstrip("/")).resolve()
        if not path.is_file() or self.served_dir not in path.parents:
            self.send_error(404, "not found")
            return
        size = path.stat().st_size
        range_hdr = self.headers.get("Range")
        start, end = 0, size - 1
        status = 200
        if range_hdr and range_hdr.startswith("bytes="):
            spec = range_hdr[len("bytes=") :]
            s_part, _, e_part = spec.partition("-")
            try:
                start = int(s_part)
                if e_part:
                    end = min(int(e_part), size - 1)
                status = 206
            except ValueError:
                pass
        self.requests_log.append({"path": self.path, "status": status, "start": start})
        self.send_response(status)
        self.send_header("Content-Length", str(end - start + 1))
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        with path.open("rb") as fh:
            fh.seek(start)
            self.wfile.write(fh.read(end - start + 1))

    def log_message(self, *_args: Any) -> None:  # keep pytest output clean
        return


@pytest.fixture
def file_server(tmp_path: Path):
    """Serve tmp_path over HTTP on 127.0.0.1; yields (base_url, root_dir)."""
    handler = type("BoundHandler", (_RangeHandler,), {"served_dir": tmp_path, "requests_log": []})
    httpd = ThreadingHTTPServer(("127.0.0.1", 0), handler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    port = httpd.server_address[1]
    yield f"http://127.0.0.1:{port}", tmp_path, handler.requests_log
    httpd.shutdown()
    thread.join(timeout=5)


# ------------------------------------------------------------------ small helpers


@pytest.fixture
def make_input_wav(tmp_path: Path):
    """Factory writing a deterministic sine-sweep WAV; returns the path."""

    def _make(name: str = "input.wav", seconds: float = 1.0, sample_rate: int = 44100) -> Path:
        frames = int(seconds * sample_rate)
        t = np.linspace(0.0, seconds, frames, dtype=np.float32)
        sweep = np.sin(2.0 * np.pi * (50.0 + 4000.0 * t / seconds) * t).astype(np.float32)
        stereo = np.stack([sweep, sweep], axis=1) * np.float32(0.4)
        path = tmp_path / name
        write_wav(path, stereo, sample_rate)
        return path

    return _make


def write_manifest(path: Path, models: list[dict[str, Any]]) -> Path:
    """Write a minimal valid manifest.json at *path* (parents created)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"schema": 1, "models": models}, indent=2), encoding="utf-8")
    return path
