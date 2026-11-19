#!/usr/bin/env python3
"""job_client.py — shared sidecar lifecycle + job helpers for harness glue scripts (WP-0133).

Behavior-preserving extraction of the serve-start/port-scan/submit/poll code that was copy-pasted
across stem_into_project.sh, midi_into_project.sh, and prompt_to_song.sh. Not engine code: dev
tooling only, stdlib-only, boring on purpose.

usage in a glue script's python phase::

    sys.path.insert(0, os.environ["OP_ROOT"] + "/harness")
    from job_client import start_sidecar, submit, wait_done
    _proc, base = start_sidecar(os.environ["OP_ROOT"], jobs_dir)
    done = wait_done(base, submit(base, "mocktrans", "transcribe", {"audio_path": wav}))
"""
from __future__ import annotations

import atexit
import json
import os
import subprocess
import time
import urllib.request


def start_sidecar(root: str, jobs_dir: str) -> tuple[subprocess.Popen[bytes], str]:
    """Start `opsidecar serve --port 0` and return (proc, base_url). Terminates at interpreter exit."""
    os.makedirs(jobs_dir, exist_ok=True)
    log_path = os.path.join(jobs_dir, "serve.log")
    log = open(log_path, "w")
    proc = subprocess.Popen(
        ["uv", "run", "opsidecar", "serve", "--port", "0", "--jobs-dir", jobs_dir],
        cwd=os.path.join(root, "ai-sidecar"), stdout=log, stderr=subprocess.STDOUT)

    def _terminate() -> None:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    atexit.register(_terminate)
    for _ in range(120):  # up to ~60s cold uv env
        try:
            with open(log_path) as f:
                for line in f:
                    if "SIDECAR_PORT=" in line:
                        port = line.split("SIDECAR_PORT=", 1)[1].split()[0]
                        return proc, f"http://127.0.0.1:{port}"
        except OSError:
            pass
        if proc.poll() is not None:
            with open(log_path) as f:
                raise SystemExit(f"FAIL: sidecar died early; log:\n{f.read()}")
        time.sleep(0.5)
    raise SystemExit("FAIL: sidecar never announced a port")


def req(method: str, url: str, body: object = None) -> dict:
    rq = urllib.request.Request(
        url,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Content-Type": "application/json"},
        method=method)
    with urllib.request.urlopen(rq) as r:
        return json.loads(r.read().decode())


def submit(base: str, backend: str, task: str, params: dict) -> str:
    resp = req("POST", base + "/jobs", {"backend": backend, "task": task, "params": params})
    job_id: str = resp["job_id"]
    return job_id


def cancel(base: str, job_id: str) -> dict:
    """POST /jobs/<id>/cancel (WP-0151); returns {"job_id", "accepted", "state"}."""
    return req("POST", f"{base}/jobs/{job_id}/cancel")


def list_jobs(base: str, state: str | None = None) -> list[dict]:
    """GET /jobs (optional exact ?state= filter), oldest submission first (WP-0151)."""
    url = base + "/jobs" + (f"?state={state}" if state else "")
    jobs: list[dict] = req("GET", url)["jobs"]
    return jobs


def wait_done(base: str, job_id: str, timeout_iters: int = 300) -> dict:
    """Poll to a terminal state; SystemExit on failed/cancelled/timeout so callers stay terse."""
    snapshot: dict = {}
    for _ in range(timeout_iters):
        snapshot = req("GET", f"{base}/jobs/{job_id}")
        if snapshot.get("state") == "done":
            return snapshot
        if snapshot.get("state") in ("failed", "cancelled"):
            raise SystemExit(f"FAIL: job {job_id} {snapshot['state']}: {snapshot.get('error')}")
        time.sleep(2)
    raise SystemExit(f"FAIL: job {job_id} timed out; last={snapshot}")


def provenance_producer(snapshot: dict, backend: str) -> str:
    """'backend@model_version' from a done snapshot's provenance file (glue convention)."""
    prov_path = snapshot["result"]["provenance_file"]
    with open(prov_path) as f:
        prov = json.load(f)
    return f"{backend}@{prov.get('model_version', '?')}"
