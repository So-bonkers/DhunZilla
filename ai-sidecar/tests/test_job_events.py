"""WP-0172 — per-job append-only event log + resumable SSE (sidecar-protocol.md).

Every state/progress mutation appends a numbered event (``seq`` 1-based, contiguous) to
the job record. ``GET /jobs/{id}/event-log`` returns events after a cursor; the SSE
stream replays from that cursor and ends right after the terminal event, so a client can
resume with ``Last-Event-ID`` without hanging or losing steps. Mock backends only: no
network, no GPU, one worker for determinism.
"""

from __future__ import annotations

import json
import threading
import time
from pathlib import Path
from typing import Any

import pytest
from fastapi.testclient import TestClient

from ai_sidecar.app import create_app

MOCKGEN_PARAMS: dict[str, Any] = {"prompt": "event log", "duration_s": 0.5, "seed": 7}


@pytest.fixture
def jobs_root(tmp_path: Path) -> Path:
    return tmp_path / "jobs"


@pytest.fixture
def client(jobs_root: Path):
    """One worker so queued/running ordering is deterministic."""
    app = create_app(jobs_dir=jobs_root, workers=1)
    with TestClient(app) as c:  # context manager runs lifespan (starts workers)
        yield c


# ---------------------------------------------------------------- helpers


def submit(client: TestClient, params: dict[str, Any] | None = None) -> str:
    resp = client.post(
        "/jobs", json={"backend": "mockgen", "task": "generate", "params": params or MOCKGEN_PARAMS}
    )
    assert resp.status_code == 202, resp.text
    return str(resp.json()["job_id"])


def wait_state(client: TestClient, job_id: str, states: set[str], timeout_iters: int = 600) -> dict:
    """Poll GET /jobs/{id} until the state is in *states* (or fail loudly)."""
    body: dict = {}
    for _ in range(timeout_iters):
        resp = client.get(f"/jobs/{job_id}")
        assert resp.status_code == 200, f"job {job_id} vanished"
        body = resp.json()
        if body.get("state") in states:
            return body
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} never reached {states}: last={body}")


def event_log(client: TestClient, job_id: str, **params: Any) -> list[dict[str, Any]]:
    resp = client.get(f"/jobs/{job_id}/event-log", params=params or None)
    assert resp.status_code == 200, resp.text
    body = resp.json()
    assert body["job_id"] == job_id
    events: list[dict[str, Any]] = body["events"]
    return events


def sse_frames(
    client: TestClient,
    job_id: str,
    *,
    params: dict[str, Any] | None = None,
    headers: dict[str, str] | None = None,
    max_frames: int = 64,
) -> list[dict[str, str]]:
    """Read SSE frames from /jobs/{id}/events until the stream ends (or fail)."""
    frames: list[dict[str, str]] = []
    current: dict[str, str] = {}
    with client.stream("GET", f"/jobs/{job_id}/events", params=params, headers=headers) as resp:
        assert resp.status_code == 200, resp.text
        assert resp.headers["content-type"].startswith("text/event-stream")
        assert resp.headers["cache-control"] == "no-cache"
        for line in resp.iter_lines():
            if line == "":
                if current:
                    frames.append(current)
                    current = {}
                    if len(frames) >= max_frames:
                        pytest.fail(f"SSE stream never terminated (>{max_frames} frames)")
                continue
            field, _, value = line.partition(":")
            if value.startswith(" "):
                value = value[1:]
            current[field] = value
    return frames


def request_body_with_timeout(
    client: TestClient,
    url: str,
    *,
    params: dict[str, Any] | None = None,
    headers: dict[str, str] | None = None,
    timeout_s: float = 10.0,
) -> str:
    """GET *url* in a helper thread; fail loudly if the response never completes."""
    box: dict[str, Any] = {}

    def run() -> None:
        try:
            resp = client.get(url, params=params, headers=headers)
            box["status"] = resp.status_code
            box["body"] = resp.text
        except Exception as exc:  # noqa: BLE001 - reported to the main thread
            box["error"] = exc

    worker = threading.Thread(target=run, daemon=True)
    worker.start()
    worker.join(timeout_s)
    assert not worker.is_alive(), f"{url} did not terminate within {timeout_s}s"
    assert "error" not in box, box["error"]
    assert box["status"] == 200
    body: str = box["body"]
    return body


# ---------------------------------------------------------------- event log content


def test_finished_mockgen_event_log_is_the_exact_sequence(client: TestClient) -> None:
    """queued, running(null), running 0.0/0.3/0.8/1.0, done — nothing collapsed away."""
    job_id = submit(client)
    final = wait_state(client, job_id, {"done", "failed"})
    assert final["state"] == "done", final.get("error")

    events = event_log(client, job_id)
    assert [(e["state"], e["progress"]) for e in events] == [
        ("queued", None),
        ("running", None),
        ("running", 0.0),
        ("running", 0.3),
        ("running", 0.8),
        ("running", 1.0),
        ("done", 1.0),
    ]
    assert [e["seq"] for e in events] == list(range(1, len(events) + 1))  # contiguous, 1-based
    assert events[0]["state"] == "queued"

    # Every event carries the snapshot fields; the last one is the final snapshot + seq.
    for event in events:
        assert event["job_id"] == job_id
        assert event["backend"] == "mockgen"
        assert event["task"] == "generate"
        assert {"state", "progress", "seq"} <= set(event)
    last = dict(events[-1])
    seq = last.pop("seq")
    assert seq == len(events)
    assert last == final

    # Progress never goes backwards (ignoring the nulls of queued/running-before-first-report).
    seen = [e["progress"] for e in events if e["progress"] is not None]
    assert seen == sorted(seen)


def test_event_log_after_query_returns_the_tail(client: TestClient) -> None:
    """?after=<n> keeps only events with seq > n; after=0 and no query are the whole log."""
    job_id = submit(client)
    wait_state(client, job_id, {"done", "failed"})
    full = event_log(client, job_id)
    assert len(full) >= 4

    tail = event_log(client, job_id, after=3)
    assert [e["seq"] for e in tail] == [e["seq"] for e in full if e["seq"] > 3]
    assert tail == full[3:]

    assert event_log(client, job_id, after=0) == full
    assert event_log(client, job_id, after=full[-1]["seq"]) == []


def test_event_log_resumes_from_last_event_id_header(client: TestClient) -> None:
    """Last-Event-ID behaves exactly like ?after= on the event-log endpoint."""
    job_id = submit(client)
    wait_state(client, job_id, {"done", "failed"})
    full = event_log(client, job_id)

    resp = client.get(f"/jobs/{job_id}/event-log", headers={"Last-Event-ID": "2"})
    assert resp.status_code == 200, resp.text
    assert resp.json()["events"] == [e for e in full if e["seq"] > 2]

    # The query wins over the header when both are present.
    resp = client.get(
        f"/jobs/{job_id}/event-log", params={"after": "5"}, headers={"Last-Event-ID": "1"}
    )
    assert resp.status_code == 200, resp.text
    assert [e["seq"] for e in resp.json()["events"]] == [6, 7]


def test_bad_cursor_is_422_json_on_both_endpoints(client: TestClient) -> None:
    """Non-integer or negative cursors are rejected as JSON bad_request (both endpoints)."""
    job_id = submit(client)
    wait_state(client, job_id, {"done", "failed"})

    for raw in ("abc", "-1", "1.5", ""):
        log = client.get(f"/jobs/{job_id}/event-log", params={"after": raw})
        assert log.status_code == 422, f"after={raw!r}: {log.status_code} {log.text}"
        err = log.json()["error"]
        assert err["code"] == "bad_request" and err["retryable"] is False
        assert err["message"] == f"after must be a non-negative integer, got '{raw}'"

        # SSE: the 422 arrives as JSON *before* any stream body.
        events = client.get(f"/jobs/{job_id}/events", params={"after": raw})
        assert events.status_code == 422, f"after={raw!r}: {events.status_code} {events.text}"
        assert events.json()["error"]["code"] == "bad_request"

    # A non-integer Last-Event-ID header is rejected too.
    resp = client.get(f"/jobs/{job_id}/event-log", headers={"Last-Event-ID": "nope"})
    assert resp.status_code == 422, resp.text
    assert resp.json()["error"]["message"] == "after must be a non-negative integer, got 'nope'"


def test_event_log_unknown_job_is_404(client: TestClient) -> None:
    resp = client.get("/jobs/nosuchjob/event-log")
    assert resp.status_code == 404
    err = resp.json()["error"]
    assert err["code"] == "unknown_job" and err["retryable"] is False


# ---------------------------------------------------------------- SSE


def test_sse_full_replay_of_finished_job_matches_the_log(client: TestClient) -> None:
    """A finished job replays every event as ``id: <seq>`` frames, then the stream ends."""
    job_id = submit(client)
    wait_state(client, job_id, {"done", "failed"})
    full = event_log(client, job_id)

    frames = sse_frames(client, job_id)
    assert [f["id"] for f in frames] == [str(e["seq"]) for e in full]
    assert all(f["event"] == "job" for f in frames)
    payloads = [json.loads(f["data"]) for f in frames]
    assert payloads == full
    assert payloads[-1]["state"] == "done"


def test_sse_resume_continues_and_past_the_end_terminates(client: TestClient) -> None:
    """Last-Event-ID resumes mid-log; after the terminal seq the body is empty, no hang."""
    job_id = submit(client)
    wait_state(client, job_id, {"done", "failed"})
    full = event_log(client, job_id)

    frames = sse_frames(client, job_id, headers={"Last-Event-ID": "3"})
    assert [f["id"] for f in frames] == ["4", "5", "6", "7"]

    last_seq = str(full[-1]["seq"])
    at_end = request_body_with_timeout(
        client, f"/jobs/{job_id}/events", headers={"Last-Event-ID": last_seq}
    )
    assert at_end == ""

    far_past = request_body_with_timeout(client, f"/jobs/{job_id}/events", params={"after": "9999"})
    assert far_past == ""


def test_sse_streams_progress_live_before_the_terminal_event(client: TestClient) -> None:
    """Streaming while the job runs still delivers every step in order and terminates."""
    job_id = submit(client)
    frames = sse_frames(client, job_id)

    seqs = [int(f["id"]) for f in frames]
    assert seqs == list(range(1, len(seqs) + 1))
    states = [json.loads(f["data"])["state"] for f in frames]
    assert states[0] == "queued"
    assert states[-1] == "done"
    assert states.count("running") >= 2  # the null plus at least one progress step were seen


# ---------------------------------------------------------------- cancellation logging


def test_queued_cancel_logs_exactly_queued_then_cancelled(
    client: TestClient, jobs_root: Path
) -> None:
    """A job cancelled while queued logs two events; its directory is gone."""
    hold_id = submit(client, {**MOCKGEN_PARAMS, "style_tags": ["mock:hold"]})
    wait_state(client, hold_id, {"running"})  # A parks the only worker
    queued_id = submit(client, {**MOCKGEN_PARAMS, "seed": 8})

    resp = client.post(f"/jobs/{queued_id}/cancel")
    assert resp.status_code == 200
    assert resp.json() == {"job_id": queued_id, "accepted": True, "state": "cancelled"}

    events = event_log(client, queued_id)
    assert [(e["state"], e["progress"]) for e in events] == [("queued", None), ("cancelled", None)]
    assert [e["seq"] for e in events] == [1, 2]
    assert "result" not in events[-1] and "error" not in events[-1]
    assert not (jobs_root / queued_id).exists()

    # Release A so teardown is quick.
    client.post(f"/jobs/{hold_id}/cancel")
    wait_state(client, hold_id, {"cancelled"})


def test_running_cancel_logs_one_cancelling_event_even_with_repeat_cancels(
    client: TestClient,
) -> None:
    """…cancelling, cancelled each exactly once however many times cancel is called."""
    job_id = submit(client, {**MOCKGEN_PARAMS, "style_tags": ["mock:hold"]})
    wait_state(client, job_id, {"running"})

    for _ in range(3):
        resp = client.post(f"/jobs/{job_id}/cancel")
        assert resp.status_code == 200
        assert resp.json()["accepted"] is True

    final = wait_state(client, job_id, {"cancelled"})
    events = event_log(client, job_id)
    states = [e["state"] for e in events]
    assert states[0] == "queued"
    assert states.count("cancelling") == 1
    assert states[-1] == "cancelled" and states.count("cancelled") == 1

    last = dict(events[-1])
    assert last.pop("seq") == len(events)
    assert last == final
