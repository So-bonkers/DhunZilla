"""WP-0151 — cooperative job cancellation + GET /jobs listing (sidecar-protocol.md).

Covers the cancel state machine end-to-end over HTTP: queued → cancelled immediately;
running → ``cancelling`` then ``cancelled`` (cooperative via mockgen's "mock:hold" hook,
and non-cooperative where the runner finalizes anyway); done/failed refuse; unknown id
404s. Also the snapshot shape of cancelled jobs and the /jobs listing order/filter.
"""

from __future__ import annotations

import time
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from ai_sidecar.app import create_app


@pytest.fixture
def jobs_root(tmp_path: Path) -> Path:
    """Where job output directories live, so tests can assert on cleanup."""
    return tmp_path / "jobs"


@pytest.fixture
def client(jobs_root: Path):
    app = create_app(jobs_dir=jobs_root, workers=2)
    with TestClient(app) as c:  # context manager runs lifespan (starts workers)
        yield c


@pytest.fixture
def client_one_worker(jobs_root: Path):
    """Single worker so a second submit is deterministically queued."""
    app = create_app(jobs_dir=jobs_root, workers=1)
    with TestClient(app) as c:
        yield c


def wait_state(client: TestClient, job_id: str, states: set[str], timeout_iters: int = 600) -> dict:
    """Poll GET /jobs/{id} until the state is in *states* (or fail loudly)."""
    body: dict = {}
    for _ in range(timeout_iters):
        resp = client.get(f"/jobs/{job_id}")
        assert resp.status_code == 200, f"job {job_id} vanished from GET /jobs/{job_id}"
        body = resp.json()
        if body.get("state") in states:
            return body
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} never reached {states}: last={body}")


def submit(client: TestClient, backend: str, task: str, params: dict) -> str:
    resp = client.post("/jobs", json={"backend": backend, "task": task, "params": params})
    assert resp.status_code == 202, resp.text
    return str(resp.json()["job_id"])


def wait_dir_gone(path: Path, timeout_iters: int = 80) -> None:
    """The cancel path deletes the job dir right after flipping state; poll briefly."""
    for _ in range(timeout_iters):
        if not path.exists():
            return
        time.sleep(0.05)
    raise AssertionError(f"job dir still present after cancel: {path}")


def assert_cancelled_snapshot(body: dict) -> None:
    """Cancelled snapshot: state cancelled, progress null, no result/error keys."""
    assert body["state"] == "cancelled"
    assert body["progress"] is None
    assert "result" not in body, body
    assert "error" not in body, body


def test_cancel_queued_job_immediate_and_clean(
    client_one_worker: TestClient, jobs_root: Path
) -> None:
    """Queued job cancels synchronously inside POST /cancel; its dir is removed."""
    from conftest import SlowGenBackend

    client_one_worker.app.state.backends["slowgen"] = SlowGenBackend(sleep_s=1.5)

    # A occupies the only worker (not cancelled — it exists to keep B queued).
    submit(
        client_one_worker, "slowgen", "generate", {"prompt": "a", "duration_s": 1.0, "seed": 1}
    )
    queued_id = submit(
        client_one_worker, "slowgen", "generate", {"prompt": "b", "duration_s": 1.0, "seed": 2}
    )

    resp = client_one_worker.post(f"/jobs/{queued_id}/cancel")
    assert resp.status_code == 200
    assert resp.json() == {"job_id": queued_id, "accepted": True, "state": "cancelled"}

    body_q = wait_state(client_one_worker, queued_id, {"cancelled"})
    assert_cancelled_snapshot(body_q)
    wait_dir_gone(jobs_root / queued_id)

    # Cancelled jobs stay listed and fetchable (spec: history is preserved).
    listed = client_one_worker.get("/jobs").json()["jobs"]
    assert queued_id in [j["job_id"] for j in listed]


def test_cancel_running_cooperative_cancelling_state(
    client: TestClient, jobs_root: Path
) -> None:
    """mockgen "mock:hold" parks a running job; cancel lands as cancelling → cancelled."""
    job_id = submit(
        client,
        "mockgen",
        "generate",
        {"prompt": "hold", "duration_s": 1.0, "seed": 3, "style_tags": ["mock:hold"]},
    )
    wait_state(client, job_id, {"running"})

    partial = jobs_root / job_id / "partial.bin"
    deadline = time.monotonic() + 10.0
    while not partial.is_file() and time.monotonic() < deadline:
        time.sleep(0.05)
    assert partial.is_file(), "mockgen hold hook never wrote partial.bin"

    resp = client.post(f"/jobs/{job_id}/cancel")
    assert resp.status_code == 200
    assert resp.json() == {"job_id": job_id, "accepted": True, "state": "cancelling"}

    body = wait_state(client, job_id, {"cancelled"})
    assert_cancelled_snapshot(body)
    wait_dir_gone(jobs_root / job_id)  # partial.bin goes with the whole directory

    # Cancelling an already-cancelled job is idempotent and still accepted.
    resp2 = client.post(f"/jobs/{job_id}/cancel")
    assert resp2.status_code == 200
    assert resp2.json() == {"job_id": job_id, "accepted": True, "state": "cancelled"}


def test_cancel_running_noncooperative_backend_still_ends_cancelled(
    client_one_worker: TestClient, jobs_root: Path
) -> None:
    """SlowGenBackend never checks cancellation; the runner must still finalize cancelled."""
    from conftest import SlowGenBackend

    client_one_worker.app.state.backends["slowgen"] = SlowGenBackend(sleep_s=1.5)
    job_id = submit(
        client_one_worker, "slowgen", "generate", {"prompt": "a", "duration_s": 1.0, "seed": 1}
    )

    wait_state(client_one_worker, job_id, {"running"})
    resp = client_one_worker.post(f"/jobs/{job_id}/cancel")
    assert resp.status_code == 200
    assert resp.json() == {"job_id": job_id, "accepted": True, "state": "cancelling"}

    # The backend finishes normally after its sleep — the runner must NOT mark it done.
    body = wait_state(client_one_worker, job_id, {"cancelled"})
    assert_cancelled_snapshot(body)
    wait_dir_gone(jobs_root / job_id)


def test_cancel_done_job_refused_artifacts_kept(
    client: TestClient, jobs_root: Path
) -> None:
    """A done job refuses cancel (accepted false); its artifacts are untouched."""
    job_id = submit(
        client, "mockgen", "generate", {"prompt": "short", "duration_s": 0.5, "seed": 5}
    )
    body = wait_state(client, job_id, {"done", "failed"})
    assert body["state"] == "done", body.get("error")

    resp = client.post(f"/jobs/{job_id}/cancel")
    assert resp.status_code == 200
    assert resp.json() == {"job_id": job_id, "accepted": False, "state": "done"}

    after = client.get(f"/jobs/{job_id}").json()
    assert after["state"] == "done" and "result" in after
    assert (jobs_root / job_id / "provenance.json").is_file()


def test_cancel_unknown_job_404(client: TestClient) -> None:
    resp = client.post("/jobs/nosuchjob/cancel")
    assert resp.status_code == 404
    err = resp.json()["error"]
    assert err["code"] == "unknown_job" and err["retryable"] is False


def test_list_jobs_empty_order_and_filter(client: TestClient) -> None:
    """GET /jobs: empty list first, then oldest-submission-first; ?state= filters exactly."""
    assert client.get("/jobs").json() == {"jobs": []}

    ids = [
        submit(
            client, "mockgen", "generate", {"prompt": f"p{i}", "duration_s": 0.5, "seed": i}
        )
        for i in range(3)
    ]
    for job_id in ids:
        assert wait_state(client, job_id, {"done", "failed"})["state"] == "done"

    listed = client.get("/jobs").json()["jobs"]
    assert [j["job_id"] for j in listed] == ids  # submission order, not completion order
    for entry in listed:
        assert entry == client.get(f"/jobs/{entry['job_id']}").json()

    done_body = client.get("/jobs", params={"state": "done"}).json()["jobs"]
    assert [j["job_id"] for j in done_body] == ids
    assert client.get("/jobs", params={"state": "queued"}).json()["jobs"] == []


def test_list_jobs_rejects_unknown_state(client: TestClient) -> None:
    resp = client.get("/jobs", params={"state": "bogus"})
    assert resp.status_code == 422
    err = resp.json()["error"]
    assert err["code"] == "bad_request" and err["retryable"] is False
