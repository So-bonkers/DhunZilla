"""API robustness (WP-0131): hostile/malformed requests must produce clean 4xx or failed jobs —
never unhandled 500s, never worker crashes, never unbounded resource grabs. Raw bodies bypass
Pydantic's happy path on purpose."""

from __future__ import annotations

import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from ai_sidecar.app import create_app


@pytest.fixture
def client(tmp_path: Path):
    app = create_app(jobs_dir=tmp_path / "jobs", workers=2)
    with TestClient(app) as c:
        yield c


def wait_terminal(c: TestClient, job_id: str, timeout_iters: int = 400) -> dict:
    for _ in range(timeout_iters):
        body = c.get(f"/jobs/{job_id}").json()
        if body.get("state") in ("done", "failed", "cancelled"):
            return body
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} never terminated: {body}")


# --- malformed bodies: must be 4xx (422 shape / 404 unknown backend), never 5xx ----------------
@pytest.mark.parametrize(
    ("payload", "expected"),
    [
        (b'{"backend":', 422),                    # truncated JSON
        (b"not json at all", 422),                # garbage
        (b"[]", 422),                             # array not object
        (b'""', 422),                             # string not object
        ({"backend": "nope", "task": "generate", "params": {}}, 404),      # unknown backend
        ({"task": "generate"}, 422),              # missing backend
        ({"backend": "mockgen"}, 422),            # missing task
        # unknown task: 4xx or accepted-then-failed, never a hang
        ({"backend": "mockgen", "task": "teleport", "params": {}}, None),
        ({"backend": "mockgen", "task": "generate", "params": "loud"}, 422),   # params not object
        ({"backend": "mockgen", "task": "generate", "params": {"prompt": None}}, None),  # null prompt: reject or fail
    ],
)
def test_malformed_submissions(client: TestClient, payload: object, expected: int | None) -> None:
    data = payload if isinstance(payload, (bytes, str)) else None
    json_body = None if data is not None else payload
    resp = client.post("/jobs", content=data, json=json_body,
                       headers={"Content-Type": "application/json"} if data is not None else {})
    assert resp.status_code < 500, f"got {resp.status_code}: {resp.text[:120]}"
    if expected is not None:
        assert resp.status_code == expected, resp.text[:200]
    elif resp.status_code == 202:  # accepted → must reach a terminal state, failed is fine
        body = wait_terminal(client, resp.json()["job_id"])
        assert body["state"] in ("failed", "done")


def test_unknown_task_reaches_failed_not_hang(client: TestClient) -> None:
    """If unknown-task submissions are accepted (202), the job must terminate 'failed' with a reason."""
    resp = client.post("/jobs", json={"backend": "mockgen", "task": "teleport", "params": {}})
    if resp.status_code == 202:
        body = wait_terminal(client, resp.json()["job_id"])
        assert body["state"] == "failed"
        assert body.get("error") or body.get("reason"), "failed job must carry a reason"
    else:
        assert 400 <= resp.status_code < 500


def test_duration_over_backend_cap_fails_cleanly(client: TestClient) -> None:
    """1e9 s must be rejected (422 or failed job) — no multi-GB allocation attempted."""
    resp = client.post("/jobs", json={"backend": "mockgen", "task": "generate",
                                      "params": {"prompt": "epic", "duration_s": 1e9, "seed": 3}})
    assert resp.status_code in (202, 422)
    if resp.status_code == 202:
        body = wait_terminal(client, resp.json()["job_id"])
        assert body["state"] == "failed"


# --- filesystem paths through transcribe-style params --------------------------------------------
def test_bad_audio_paths_fail_cleanly(client: TestClient, tmp_path: Path) -> None:
    from ai_sidecar.wav import write_wav
    import numpy as np

    good = tmp_path / "good.wav"   # sanity artifact; bad paths below must all fail cleanly
    write_wav(good, np.zeros((1000, 2), dtype=np.float32), 48000)

    for label, path in [("missing", str(tmp_path / "nope.wav")),
                        ("directory", str(tmp_path)),
                        ("non_wav", "/etc/hostname")]:
        resp = client.post("/jobs", json={"backend": "mocktrans", "task": "transcribe",
                                          "params": {"audio_path": path}})
        assert resp.status_code in (202, 422), f"{label}: {resp.status_code}"
        if resp.status_code == 202:
            body = wait_terminal(client, resp.json()["job_id"])
            assert body["state"] == "failed", f"{label} must not succeed: {body}"


# --- job lifecycle edges ---------------------------------------------------------------------
def test_lifecycle_edges(client: TestClient) -> None:
    assert client.get("/jobs/does-not-exist").status_code == 404
    assert client.post("/jobs/does-not-exist/cancel").status_code == 404

    resp = client.post("/jobs", json={"backend": "mockgen", "task": "generate",
                                      "params": {"prompt": "x", "duration_s": 0.5, "seed": 7}})
    assert resp.status_code == 202
    jid = resp.json()["job_id"]
    body = wait_terminal(client, jid)
    assert body["state"] == "done"
    # terminal-state cancels and double-fetches must be clean, not 5xx
    assert client.post(f"/jobs/{jid}/cancel").status_code in (200, 409, 404, 422)
    again = client.get(f"/jobs/{jid}")
    assert again.status_code == 200 and again.json()["state"] == "done"
