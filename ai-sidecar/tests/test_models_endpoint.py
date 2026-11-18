"""WP-0005 acceptance: GET /models + POST /models/{id}/download via TestClient —
tier/availability/status surfaced correctly; license gate enforced end-to-end."""

from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Any

import pytest
from conftest import write_manifest
from fastapi.testclient import TestClient

from ai_sidecar.app import create_app

PAYLOAD = b"endpoint fixture weights " * 8192


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _entry(
    model_id: str, tier: str, url: str, digest: str, *, status: str = "live"
) -> dict[str, Any]:
    entry: dict[str, Any] = {
        "id": model_id,
        "backend": "demo",
        "task": "stem_separation",
        "sources": [{"url": url, "sha256": digest, "size_bytes": None, "hash_verified": True}],
        "cache_dir": "",  # filled by caller with an absolute tmp path
        "license": {
            "code": "MIT",
            "weights_tier": tier,
            "weights_spdx": "NOASSERTION",
            "notes": f"{model_id} fixture notes",
            "verified_on": "2026-11-18",
        },
        "default_enabled": False,
        "status": status,
        "capabilities_hint": {},
    }
    return entry


@pytest.fixture
def models_client(tmp_path: Path, file_server):
    """App wired to a 3-model manifest (permissive / personal-use-ok / awaiting)."""
    base_url, root, _ = file_server
    (root / "weights.bin").write_bytes(PAYLOAD)

    entries = [
        _entry("free-model", "permissive", f"{base_url}/weights.bin", sha(PAYLOAD)),
        _entry("gated-model", "personal-use-ok", f"{base_url}/weights.bin", sha(PAYLOAD)),
        _entry("ace-next", "commercial-needs-contact", "", "1" * 64, status="awaiting-weights"),
    ]
    entries[0]["cache_dir"] = str(tmp_path / "c1")
    entries[1]["cache_dir"] = str(tmp_path / "c2")
    entries[2]["cache_dir"] = str(tmp_path / "c3")
    entries[2]["sources"] = []
    entries[2]["capabilities_hint"] = {
        "unverified_claims": True,
        "announcement_url": "https://example.com/next",
        "announced_on": "2026-11-18",
    }
    manifest_path = write_manifest(tmp_path / "models" / "manifest.json", entries)

    app = create_app(jobs_dir=tmp_path / "jobs", manifest_path=manifest_path)
    with TestClient(app) as client:
        yield client, base_url


def test_models_endpoint_lists_tier_availability_status(models_client) -> None:
    client, _ = models_client
    resp = client.get("/models")
    assert resp.status_code == 200
    rows = {m["id"]: m for m in resp.json()["models"]}

    assert rows["free-model"]["license_tier"] == "permissive"
    assert rows["free-model"]["requires_accept"] is False
    assert rows["gated-model"]["license_tier"] == "personal-use-ok"
    assert rows["gated-model"]["requires_accept"] is True
    assert rows["ace-next"]["status"] == "awaiting-weights"
    for row in rows.values():
        assert row["local_available"] is False
        assert {"verified_on", "default_enabled", "hash_verified"} <= set(row)


def test_download_endpoint_license_gate(models_client, tmp_path: Path) -> None:
    client, _ = models_client

    # personal-use-ok refused without accept (WP-0004 acceptance criterion too).
    resp = client.post("/models/gated-model/download", json={})
    assert resp.status_code == 403
    err = resp.json()["error"]
    assert err["code"] == "license_not_accepted" and "accept" in err["message"].lower()

    # ...and succeeds with accept.
    resp = client.post("/models/gated-model/download", json={"accept": True})
    assert resp.status_code == 200
    body = resp.json()
    assert body["status"] == "downloaded" and body["sha256_verified"] is True
    assert Path(body["files"][0]).read_bytes() == PAYLOAD

    # Availability now visible through GET /models.
    rows = {m["id"]: m for m in client.get("/models").json()["models"]}
    assert rows["gated-model"]["local_available"] is True


def test_download_permissive_without_accept(models_client) -> None:
    client, _ = models_client
    resp = client.post("/models/free-model/download", json={})
    assert resp.status_code == 200 and resp.json()["status"] == "downloaded"


def test_download_awaiting_weights_actionable(models_client) -> None:
    client, _ = models_client
    resp = client.post("/models/ace-next/download", json={"accept": True})
    assert resp.status_code == 409
    err = resp.json()["error"]
    assert err["code"] == "model_awaiting_weights"
    assert "not released" in err["message"] and "https://example.com/next" in err["message"]


def test_download_unknown_model(models_client) -> None:
    client, _ = models_client
    resp = client.post("/models/nope/download", json={"accept": True})
    assert resp.status_code == 404
    assert resp.json()["error"]["code"] == "unknown_model"
