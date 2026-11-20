"""Stem bundle tests (WP-0174, docs/spec/stem-bundle.md).

Small synthetic WAVs written with ``ai_sidecar.wav.write_wav`` plus the mock backends —
no network, no heavy deps. Covers: bundle content/sort order, sha256, peak/rms,
the three contract errors, and end-to-end behaviour through the job runner.
"""

from __future__ import annotations

import hashlib
import json
import math
import time
from pathlib import Path
from typing import Any

import numpy as np
import pytest
from fastapi.testclient import TestClient

from ai_sidecar.app import create_app
from ai_sidecar.base import InvalidRequest
from ai_sidecar.stem_bundle import (
    BUNDLE_FILENAME,
    BUNDLE_SCHEMA,
    build_stem_bundle,
    write_stem_bundle,
)
from ai_sidecar.wav import write_wav

SR = 44100


def _write_stem(path: Path, samples: np.ndarray, sample_rate: int = SR) -> Path:
    write_wav(path, samples, sample_rate)
    return path


# ------------------------------------------------------------------ bundle content


def test_bundle_content_and_sort_order(tmp_path: Path) -> None:
    """3 stems inserted out of order → keys exact, stems sorted by name, shared format."""
    a = _write_stem(tmp_path / "vocals.wav", np.full((2048, 2), 0.25, dtype=np.float32))
    b = _write_stem(tmp_path / "bass.wav", np.full((2048, 2), -0.125, dtype=np.float32))
    c = _write_stem(tmp_path / "drums.wav", np.full((2048, 2), 0.5, dtype=np.float32))

    bundle = build_stem_bundle({"vocals": a, "bass": b, "drums": c})

    assert set(bundle) == {"schema", "sample_rate", "channels", "frames", "stems"}
    assert bundle["schema"] == BUNDLE_SCHEMA == 1
    assert (bundle["sample_rate"], bundle["channels"], bundle["frames"]) == (SR, 2, 2048)
    assert [s["name"] for s in bundle["stems"]] == ["bass", "drums", "vocals"]
    for stem in bundle["stems"]:
        assert set(stem) == {"name", "file", "sha256", "frames", "peak", "rms"}
        assert stem["file"] == f"{stem['name']}.wav"  # basename inside the job dir
        assert stem["frames"] == 2048


def test_sha256_matches_file_bytes(tmp_path: Path) -> None:
    """Each entry's sha256 is the hex digest of that WAV file's bytes."""
    samples = np.sin(np.linspace(0.0, 100.0, 1000, dtype=np.float64)).astype(np.float32)
    stereo = np.stack([samples, -samples], axis=1)
    path = _write_stem(tmp_path / "bass.wav", stereo)

    entry = build_stem_bundle({"bass": path})["stems"][0]

    assert entry["sha256"] == hashlib.sha256(path.read_bytes()).hexdigest()


def test_peak_and_rms_of_known_sine_and_silence(tmp_path: Path) -> None:
    """Known levels: a crest-aligned 0.5 sine → peak 0.5, rms 0.5/√2 exactly; silence → 0.0."""
    # A sine sampled at 4 points per period lands exactly on its crest/nadir: ±0.5, 0.
    crest = np.tile(np.array([0.5, 0.0, -0.5, 0.0], dtype=np.float32), 256)
    tone = _write_stem(tmp_path / "tone.wav", np.stack([crest, crest], axis=1))

    entry = build_stem_bundle({"tone": tone})["stems"][0]
    assert isinstance(entry["peak"], float) and isinstance(entry["rms"], float)
    assert entry["peak"] == 0.5
    assert entry["rms"] == round(0.5 / math.sqrt(2.0), 6) == 0.353553

    # A smooth 1 kHz sine over exactly 100 periods: rms is the analytic A/√2; the peak sits
    # just below A because no sample lands on the crest (44.1 samples per period).
    frames = 4410
    t = np.arange(frames, dtype=np.float64) / SR
    smooth = (0.5 * np.sin(2.0 * np.pi * 1000.0 * t)).astype(np.float32)
    smooth_path = _write_stem(tmp_path / "smooth.wav", np.stack([smooth, smooth], axis=1))
    loud = build_stem_bundle({"smooth": smooth_path})["stems"][0]
    assert loud["rms"] == round(0.5 / math.sqrt(2.0), 6)  # rounding to 6 decimals is the only delta
    assert 0.499 < loud["peak"] <= 0.5

    silence = _write_stem(tmp_path / "silence.wav", np.zeros((1024, 2), dtype=np.float32))
    quiet = build_stem_bundle({"silence": silence})["stems"][0]
    assert quiet["peak"] == 0.0 and quiet["rms"] == 0.0


def test_write_stem_bundle_is_byte_stable(tmp_path: Path) -> None:
    """write_stem_bundle → <out_dir>/stems.json, indent=2 + sort_keys + trailing newline."""
    path = _write_stem(tmp_path / "drums.wav", np.full((512, 2), 0.125, dtype=np.float32))

    out = write_stem_bundle(tmp_path, {"drums": path})

    assert out == tmp_path / BUNDLE_FILENAME and out.is_file()
    written = out.read_bytes()
    assert written.endswith(b"\n")
    assert written == (json.dumps(json.loads(written), indent=2, sort_keys=True) + "\n").encode()


# ------------------------------------------------------------------ contract errors


def test_error_no_stems() -> None:
    with pytest.raises(InvalidRequest) as exc:
        build_stem_bundle({})
    assert str(exc.value) == "separate job produced no stems"


def test_error_not_a_readable_wav(tmp_path: Path) -> None:
    """Garbage bytes and a missing file both raise the same contract message."""
    junk = tmp_path / "junk.wav"
    junk.write_bytes(b"this is not a RIFF/WAVE file at all")

    with pytest.raises(InvalidRequest) as exc:
        build_stem_bundle({"junk": junk})
    assert str(exc.value).startswith("stem 'junk' is not a readable WAV: ")

    missing = tmp_path / "ghost.wav"
    with pytest.raises(InvalidRequest) as exc2:
        build_stem_bundle({"ghost": missing})
    assert str(exc2.value).startswith("stem 'ghost' is not a readable WAV: ")


def test_error_mismatched_stem_shape(tmp_path: Path) -> None:
    """Every stem must share the first (alphabetically smallest) stem's shape."""
    good = _write_stem(tmp_path / "a.wav", np.zeros((1000, 2), dtype=np.float32))
    short = _write_stem(tmp_path / "b.wav", np.zeros((500, 2), dtype=np.float32))

    with pytest.raises(InvalidRequest) as exc:
        build_stem_bundle({"b": short, "a": good})  # insertion order must not matter
    assert str(exc.value) == (
        "stem 'b' has (sample_rate, channels, frames) = (44100, 2, 500), expected (44100, 2, 1000)"
    )

    other_sr = _write_stem(tmp_path / "c.wav", np.zeros((1000, 2), dtype=np.float32), 22050)
    with pytest.raises(InvalidRequest) as exc2:
        build_stem_bundle({"a": good, "c": other_sr})
    assert str(exc2.value) == (
        "stem 'c' has (sample_rate, channels, frames) = (22050, 2, 1000), expected (44100, 2, 1000)"
    )


# ------------------------------------------------------------------ end to end (jobs)


@pytest.fixture
def client(tmp_path: Path):
    """TestClient over a real app with the discovered mock backends; jobs in tmp_path."""
    app = create_app(jobs_dir=tmp_path / "jobs", workers=2)
    with TestClient(app) as c:  # context manager runs lifespan (starts workers)
        yield c


def _wait_done(client: TestClient, job_id: str, timeout_iters: int = 600) -> dict[str, Any]:
    body: dict[str, Any] = {}
    for _ in range(timeout_iters):
        body = client.get(f"/jobs/{job_id}").json()
        if body.get("state") in {"done", "failed", "cancelled"}:
            return body
        time.sleep(0.05)
    raise AssertionError(f"job {job_id} never terminated: last={body}")


def _submit(client: TestClient, backend: str, task: str, params: dict[str, Any]) -> str:
    resp = client.post("/jobs", json={"backend": backend, "task": task, "params": params})
    assert resp.status_code == 202, resp.text
    return str(resp.json()["job_id"])


def test_mocksep_job_writes_validated_bundle(client: TestClient, make_input_wav) -> None:
    """A done separate job carries stems_file; the bundle lists all four mock stems."""
    input_wav = make_input_wav()
    job_id = _submit(client, "mocksep", "separate", {"audio_path": str(input_wav)})
    body = _wait_done(client, job_id)

    assert body["state"] == "done", body.get("error")
    stems_file = Path(body["result"]["stems_file"])
    assert stems_file.parent.name == job_id and stems_file.name == BUNDLE_FILENAME
    bundle = json.loads(stems_file.read_text())
    assert [s["name"] for s in bundle["stems"]] == ["bass", "drums", "other", "vocals"]
    assert (bundle["sample_rate"], bundle["channels"]) == (44100, 2)

    # The bundle agrees with the artifacts it describes.
    artifacts = body["result"]["artifacts"]
    assert {s["name"] for s in bundle["stems"]} == set(artifacts)
    for stem in bundle["stems"]:
        file_path = Path(artifacts[stem["name"]])
        assert stem["file"] == file_path.name
        assert stem["sha256"] == hashlib.sha256(file_path.read_bytes()).hexdigest()


def test_mocksep_two_runs_identical_bundle_bytes(client: TestClient, make_input_wav) -> None:
    """Deterministic backend → byte-identical stems.json across two runs of the same input."""
    input_wav = make_input_wav()
    bytes_by_job: list[bytes] = []
    for _ in range(2):
        job_id = _submit(client, "mocksep", "separate", {"audio_path": str(input_wav)})
        body = _wait_done(client, job_id)
        assert body["state"] == "done", body.get("error")
        bytes_by_job.append(Path(body["result"]["stems_file"]).read_bytes())

    assert bytes_by_job[0] == bytes_by_job[1]


def test_mockgen_job_has_no_bundle(client: TestClient) -> None:
    """generate jobs are untouched: no stems_file key and no stems.json on disk."""
    job_id = _submit(
        client, "mockgen", "generate", {"prompt": "lofi", "duration_s": 1.0, "seed": 3}
    )
    body = _wait_done(client, job_id)

    assert body["state"] == "done", body.get("error")
    assert "stems_file" not in body["result"]
    out_dir = Path(body["result"]["provenance_file"]).parent
    assert not (out_dir / BUNDLE_FILENAME).exists()
    assert Path(body["result"]["artifacts"]["audio"]).is_file()
