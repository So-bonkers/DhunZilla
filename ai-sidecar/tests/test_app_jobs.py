"""App + job-runner HTTP behavior tests (TestClient): protocol surface of WP-0004."""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from ai_sidecar.app import create_app
from ai_sidecar.registry import discover_backends


@pytest.fixture
def client(tmp_path: Path):
    """TestClient over a real app with discovered mocks; jobs land in tmp_path."""
    app = create_app(jobs_dir=tmp_path / "jobs", workers=2)
    with TestClient(app) as c:  # context manager runs lifespan (starts workers)
        yield c


@pytest.fixture
def client_one_worker(tmp_path: Path):
    """Single-worker app so a second submit is deterministically queued."""
    app = create_app(jobs_dir=tmp_path / "jobs1", workers=1)
    with TestClient(app) as c:
        yield c


def wait_state(client: TestClient, job_id: str, states: set[str], timeout_iters: int = 600) -> dict:
    """Poll GET /jobs/{id} until the state is in *states* (or fail loudly)."""
    body: dict = {}
    for _ in range(timeout_iters):
        body = client.get(f"/jobs/{job_id}").json()
        if body.get("state") in states:
            return body
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} never reached {states}: last={body}")


def submit(client: TestClient, backend: str, task: str, params: dict) -> str:
    resp = client.post("/jobs", json={"backend": backend, "task": task, "params": params})
    assert resp.status_code == 202, resp.text
    return str(resp.json()["job_id"])


def test_health_lists_backends(client: TestClient) -> None:
    resp = client.get("/health")
    assert resp.status_code == 200
    body = resp.json()
    assert body["ok"] is True
    ids = {b["id"] for b in body["backends"]}
    assert {"mockgen", "mocksep"} <= ids
    mockgen = next(b for b in body["backends"] if b["id"] == "mockgen")
    assert mockgen["task"] == "generate"
    assert mockgen["capabilities"]["text2loop"] is True
    assert mockgen["capabilities"]["stem_separation"] is False
    assert mockgen["health"]["status"] == "ok"


def test_mockgen_job_done_with_artifacts_and_provenance(client: TestClient) -> None:
    job_id = submit(
        client,
        "mockgen",
        "generate",
        {"prompt": "chill loop", "duration_s": 1.5, "seed": 7},
    )
    body = wait_state(client, job_id, {"done", "failed"})
    assert body["state"] == "done", body.get("error")
    artifacts = body["result"]["artifacts"]
    audio = Path(artifacts["audio"])
    assert audio.is_file() and audio.parent.name == job_id
    prov = json.loads((audio.parent / "provenance.json").read_text())
    assert prov["backend_id"] == "mockgen" and prov["seed"] == 7


def test_mocksep_job_streams_and_completes(client: TestClient, make_input_wav) -> None:
    input_wav = make_input_wav()
    job_id = submit(client, "mocksep", "separate", {"audio_path": str(input_wav)})

    # SSE: stream events until terminal (spec allows poll-fed streams).
    seen_states: list[str] = []
    with client.stream("GET", f"/jobs/{job_id}/events") as stream:
        assert stream.headers["content-type"].startswith("text/event-stream")
        for line in stream.iter_lines():
            if line.startswith("data: "):
                payload = json.loads(line[len("data: ") :])
                seen_states.append(payload["state"])
                if payload["state"] in {"done", "failed", "cancelled"}:
                    break
            if len(seen_states) > 50:
                pytest.fail("SSE stream never terminated")
    assert seen_states[-1] == "done", seen_states

    body = client.get(f"/jobs/{job_id}").json()
    stems = set(body["result"]["artifacts"])
    assert {"vocals", "drums", "bass", "other"} <= stems


def test_cancel_queued_and_running(client_one_worker: TestClient) -> None:
    """With one worker: job A runs, job B queues → both cancellable; B must cancel."""
    from conftest import SlowGenBackend

    app_backends = client_one_worker.app.state.backends  # same dict the runner holds
    app_backends["slowgen"] = SlowGenBackend(sleep_s=1.5)

    running_id = submit(
        client_one_worker, "slowgen", "generate",
        {"prompt": "a", "duration_s": 1.0, "seed": 1},
    )
    queued_id = submit(
        client_one_worker, "slowgen", "generate",
        {"prompt": "b", "duration_s": 1.0, "seed": 2},
    )

    # B is queued (single worker busy with A). Cancel it immediately.
    resp = client_one_worker.post(f"/jobs/{queued_id}/cancel")
    assert resp.status_code == 200 and resp.json()["accepted"] is True
    body_q = wait_state(client_one_worker, queued_id, {"cancelled"})
    assert body_q["state"] == "cancelled"

    # A is running; cancel is best-effort but the runner finalizes as cancelled.
    wait_state(client_one_worker, running_id, {"running"}, timeout_iters=400)
    resp = client_one_worker.post(f"/jobs/{running_id}/cancel")
    assert resp.status_code == 200 and resp.json()["accepted"] is True
    body_r = wait_state(client_one_worker, running_id, {"cancelled", "done"})
    assert body_r["state"] == "cancelled"


def test_error_shapes(client: TestClient) -> None:
    resp = client.post("/jobs", json={"backend": "nope", "task": "generate", "params": {}})
    assert resp.status_code == 404
    err = resp.json()["error"]
    assert err["code"] == "unknown_backend" and err["retryable"] is False

    resp = client.post(
        "/jobs", json={"backend": "mockgen", "task": "separate", "params": {"audio_path": "/x"}}
    )
    assert resp.status_code == 422
    assert resp.json()["error"]["code"] == "bad_request"

    resp = client.get("/jobs/doesnotexist")
    assert resp.status_code == 404 and resp.json()["error"]["code"] == "unknown_job"

    # Missing required param (seed): accepted at enqueue, fails as bad_request downstream.
    job_id = submit(client, "mockgen", "generate", {"prompt": "x"})
    body = wait_state(client, job_id, {"failed"})
    assert body["error"]["code"] == "bad_request"

    # Unknown task token → 422 at submit time.
    resp = client.post("/jobs", json={"backend": "mockgen", "task": "dance", "params": {}})
    assert resp.status_code == 422 and resp.json()["error"]["code"] == "bad_request"


def test_separate_missing_file_fails_structured(client: TestClient) -> None:
    job_id = submit(
        client, "mocksep", "separate", {"audio_path": "/nonexistent/input.wav"}
    )
    body = wait_state(client, job_id, {"failed"})
    assert body["state"] == "failed"
    assert body["error"]["code"] == "bad_request"


def test_registry_discovered_backends_are_the_mocks() -> None:
    assert {"mockgen", "mocksep"} <= set(discover_backends())
