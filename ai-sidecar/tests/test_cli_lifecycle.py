"""Lifecycle test per spec/sidecar-protocol.md: spawn with --port 0, read
SIDECAR_PORT=<n> from stdout, health-check over HTTP, terminate cleanly."""

from __future__ import annotations

import subprocess
import sys
import time
from pathlib import Path

import httpx


def test_serve_announces_port_and_answers_health(tmp_path: Path) -> None:
    proc = subprocess.Popen(
        [sys.executable, "-m", "ai_sidecar", "serve", "--port", "0",
         "--jobs-dir", str(tmp_path / "jobs"), "--log-level", "warning"],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        port: int | None = None
        deadline = time.time() + 30.0
        assert proc.stdout is not None
        for line in proc.stdout:
            if line.startswith("SIDECAR_PORT="):
                port = int(line.strip().split("=", 1)[1])
                break
            if time.time() > deadline:
                break
        assert port is not None, "SIDECAR_PORT=<n> line never appeared on stdout"

        with httpx.Client(base_url=f"http://127.0.0.1:{port}", timeout=10.0) as client:
            # Spec lifecycle: health-check with backoff (server announces port before
            # uvicorn fully starts; kernel backlog covers most of it, retry the rest).
            health = None
            for _ in range(50):
                try:
                    health = client.get("/health")
                    if health.status_code == 200:
                        break
                except httpx.ConnectError:
                    pass
                time.sleep(0.1)
            assert health is not None and health.status_code == 200
            assert health.json()["ok"] is True
            resp = client.post(
                "/jobs",
                json={"backend": "mockgen", "task": "generate",
                      "params": {"prompt": "cli", "duration_s": 0.5, "seed": 11}},
            )
            assert resp.status_code == 202
            job_id = resp.json()["job_id"]
            for _ in range(200):
                body = client.get(f"/jobs/{job_id}").json()
                if body["state"] in {"done", "failed", "cancelled"}:
                    break
                time.sleep(0.05)
            assert body["state"] == "done"
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:  # pragma: no cover
            proc.kill()
            proc.wait(timeout=5)


def test_module_alias_and_console_script_version() -> None:
    """`python -m opsidecar --version` and the `opsidecar` console script both work."""
    alias = subprocess.run(
        [sys.executable, "-m", "opsidecar", "--version"], capture_output=True, text=True, timeout=30
    )
    assert alias.returncode == 0 and "opsidecar" in alias.stdout
    script = Path(sys.executable).parent / "opsidecar"
    if script.is_file():
        run = subprocess.run([str(script), "--version"], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0 and "opsidecar" in run.stdout
