"""WP-0005 model manager tests: downloader (happy/resume/mismatch/quarantine),
license tri-state gates, manifest schema validation. All downloads hit a local
127.0.0.1 file server — no external network."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import pytest
from conftest import write_manifest

from ai_sidecar.model_manager import (
    DownloadFailed,
    HashMismatch,
    LicenseNotAccepted,
    ManifestError,
    ModelAwaitingWeights,
    ModelManager,
    ModelNotFound,
)


def sha256_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


PAYLOAD = b"openpattern synthetic weights payload - tiny on purpose. " * 4096  # ~216 KB


def make_entry(
    *,
    model_id: str = "m1",
    tier: str = "personal-use-ok",
    url: str,
    sha256: str,
    size_bytes: int | None = None,
    status: str = "live",
    default_enabled: bool = False,
    cache_dir: str,
) -> dict[str, Any]:
    return {
        "id": model_id,
        "backend": "demo",
        "task": "stem_separation",
        "sources": [
            {"url": url, "sha256": sha256, "size_bytes": size_bytes, "hash_verified": True}
        ],
        "cache_dir": cache_dir,
        "license": {
            "code": "MIT",
            "weights_tier": tier,
            "weights_spdx": "NOASSERTION",
            "notes": "test fixture license notes",
            "verified_on": "2026-11-18",
        },
        "default_enabled": default_enabled,
        "status": status,
        "capabilities_hint": {},
    }


@pytest.fixture
def server_content(file_server):
    """Write PAYLOAD into the served dir; return (base_url, root, request_log)."""
    base_url, root, log = file_server
    (root / "weights.bin").write_bytes(PAYLOAD)
    return base_url, root, log


def mgr_for(tmp_path: Path, entries: list[dict[str, Any]]) -> ModelManager:
    manifest_path = write_manifest(tmp_path / "models" / "manifest.json", entries)
    return ModelManager(manifest_path)


# ---------------------------------------------------------------- downloader


def test_download_happy_path_with_accept(tmp_path: Path, server_content) -> None:
    base_url, _, log = server_content
    cache = tmp_path / "cache"
    entry = make_entry(url=f"{base_url}/weights.bin", sha256=sha256_of(PAYLOAD),
                       size_bytes=len(PAYLOAD), cache_dir=str(cache))
    mgr = mgr_for(tmp_path, [entry])

    assert mgr.local_available(mgr.get_entry("m1")) is False
    outcome = mgr.download("m1", accept=True)
    dest = cache / "weights.bin"
    assert dest.is_file() and dest.read_bytes() == PAYLOAD
    assert outcome["files"] == [str(dest)]
    assert not (cache / "weights.bin.part").exists()
    assert mgr.local_available(mgr.get_entry("m1")) is True

    # Second download: already present → no new HTTP request.
    n_before = len(log)
    mgr.download("m1", accept=True)
    assert len(log) == n_before


def test_download_range_resume(tmp_path: Path, server_content) -> None:
    """Half-written .part resumes with Range; final file verifies."""
    base_url, _, log = server_content
    cache = tmp_path / "cache"
    cache.mkdir(parents=True)
    half = len(PAYLOAD) // 2
    (cache / "weights.bin.part").write_bytes(PAYLOAD[:half])

    entry = make_entry(url=f"{base_url}/weights.bin", sha256=sha256_of(PAYLOAD),
                       cache_dir=str(cache))
    mgr = mgr_for(tmp_path, [entry])
    outcome = mgr.download("m1", accept=True)

    dest = Path(outcome["files"][0])
    assert dest.read_bytes() == PAYLOAD
    resumed = [r for r in log if r["status"] == 206]
    assert resumed, f"expected a 206 partial response, got {log}"


def test_sha256_mismatch_quarantines_no_retry(tmp_path: Path, server_content) -> None:
    base_url, _, log = server_content
    cache = tmp_path / "cache"
    entry = make_entry(url=f"{base_url}/weights.bin", sha256="0" * 64, cache_dir=str(cache))
    mgr = mgr_for(tmp_path, [entry])

    with pytest.raises(HashMismatch, match="sha256 mismatch"):
        mgr.download("m1", accept=True)
    assert (cache / "weights.bin.quarantined").is_file()
    assert not (cache / "weights.bin").exists()
    assert not (cache / "weights.bin.part").exists()
    assert len(log) == 1, "mismatch must NOT trigger a retry loop"


def test_download_http_failure(tmp_path: Path, server_content) -> None:
    base_url, _, _ = server_content
    entry = make_entry(url=f"{base_url}/missing.bin", sha256=sha256_of(PAYLOAD),
                       cache_dir=str(tmp_path / "cache"))
    mgr = mgr_for(tmp_path, [entry])
    with pytest.raises(DownloadFailed, match="HTTP 404"):
        mgr.download("m1", accept=True)


# ---------------------------------------------------------------- license gates


def test_personal_use_ok_requires_accept(tmp_path: Path, server_content) -> None:
    base_url, _, log = server_content
    entry = make_entry(url=f"{base_url}/weights.bin", sha256=sha256_of(PAYLOAD),
                       tier="personal-use-ok", cache_dir=str(tmp_path / "cache"))
    mgr = mgr_for(tmp_path, [entry])
    with pytest.raises(LicenseNotAccepted, match="accept"):
        mgr.download("m1")
    assert len(log) == 0, "refusal must happen before any network request"
    mgr.download("m1", accept=True)  # explicit accept → allowed


def test_permissive_allowed_without_accept(tmp_path: Path, server_content) -> None:
    base_url, _, _ = server_content
    entry = make_entry(url=f"{base_url}/weights.bin", sha256=sha256_of(PAYLOAD),
                       tier="permissive", cache_dir=str(tmp_path / "cache"))
    mgr = mgr_for(tmp_path, [entry])
    outcome = mgr.download("m1")  # no accept needed
    assert Path(outcome["files"][0]).read_bytes() == PAYLOAD


def test_commercial_needs_contact_never_auto_downloads(tmp_path: Path, server_content) -> None:
    base_url, _, log = server_content
    entry = make_entry(url=f"{base_url}/weights.bin", sha256=sha256_of(PAYLOAD),
                       tier="commercial-needs-contact", cache_dir=str(tmp_path / "cache"))
    mgr = mgr_for(tmp_path, [entry])
    with pytest.raises(LicenseNotAccepted, match="contact"):
        mgr.download("m1")
    with pytest.raises(LicenseNotAccepted, match="never auto-downloaded"):
        mgr.download("m1", accept=True)  # even explicit accept does not bypass this tier
    assert len(log) == 0


def test_awaiting_weights_refused_with_actionable_message(tmp_path: Path) -> None:
    entry = make_entry(
        url="http://unused.invalid/none.bin", sha256="1" * 64, status="awaiting-weights",
        cache_dir=str(tmp_path / "cache"),
    )
    entry["sources"] = []
    entry["capabilities_hint"] = {
        "unverified_claims": True,
        "announcement_url": "https://example.com/release-notes",
        "announced_on": "2026-11-18",
    }
    mgr = mgr_for(tmp_path, [entry])
    with pytest.raises(ModelAwaitingWeights) as excinfo:
        mgr.download("m1", accept=True)
    message = str(excinfo.value)
    assert "not released" in message
    assert "https://example.com/release-notes" in message
    assert "UNVERIFIED" in message


def test_unknown_model_lists_known_ids(tmp_path: Path, server_content) -> None:
    mgr = mgr_for(tmp_path, [make_entry(url="http://x/y", sha256="a" * 64,
                                        cache_dir=str(tmp_path / "c"))])
    with pytest.raises(ModelNotFound, match="m1"):
        mgr.get_entry("nope")


# ---------------------------------------------------------------- manifest schema


def _valid_entry() -> dict[str, Any]:
    return make_entry(url="https://example.com/w.bin", sha256="a" * 64,
                      cache_dir="/tmp/whatever")


@pytest.mark.parametrize(
    "mutate, match",
    [
        (lambda m: m.update({"schema": 2}), "schema"),
        (lambda m: m["models"][0]["license"].pop("verified_on"), "verified_on"),
        (lambda m: m["models"][0]["license"].update({"weights_tier": "GPL-ish"}), "weights_tier"),
        (lambda m: m["models"][0].update({"default_enabled": True}), "permissive"),
        (lambda m: m["models"][0]["sources"][0].pop("sha256"), "sha256"),
        (lambda m: m["models"][0]["sources"][0].update({"sha256": "deadbeef"}), "sha256"),
        (lambda m: m["models"][0]["sources"][0].update({"url": "ftp://example.com/w.bin"}), "url"),
        (lambda m: m["models"][0].update({"unknown_field": 1}), "unknown_field"),
        (lambda m: m["models"].append(m["models"][0].copy()), "duplicate"),
        (lambda m: m["models"][0].update({"sources": []}), "source"),
    ],
)
def test_manifest_rejects_bad_documents(tmp_path: Path, mutate, match: str) -> None:
    doc = {"schema": 1, "models": [_valid_entry()]}
    mutate(doc)
    path = tmp_path / "models" / "manifest.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(doc), encoding="utf-8")
    with pytest.raises(ManifestError, match=match):
        ModelManager(path).manifest()


def test_manifest_missing_file_actionable(tmp_path: Path) -> None:
    mgr = ModelManager(tmp_path / "nope" / "manifest.json")
    with pytest.raises(ManifestError, match="cannot read"):
        mgr.manifest()


def test_view_reports_tier_availability_status(tmp_path: Path, server_content) -> None:
    base_url, _, _ = server_content
    cache = tmp_path / "cache"
    entry = make_entry(url=f"{base_url}/weights.bin", sha256=sha256_of(PAYLOAD),
                       tier="permissive", cache_dir=str(cache))
    mgr = mgr_for(tmp_path, [entry])
    rows = {r["id"]: r for r in mgr.view()}
    assert rows["m1"]["license_tier"] == "permissive"
    assert rows["m1"]["local_available"] is False
    assert rows["m1"]["requires_accept"] is False
    assert rows["m1"]["status"] == "live"
    mgr.download("m1")
    rows = {r["id"]: r for r in mgr.view()}
    assert rows["m1"]["local_available"] is True


def test_local_available_false_on_size_mismatch(tmp_path: Path, server_content) -> None:
    base_url, _, _ = server_content
    cache = tmp_path / "cache"
    entry = make_entry(url=f"{base_url}/weights.bin", sha256=sha256_of(PAYLOAD),
                       tier="permissive", size_bytes=len(PAYLOAD), cache_dir=str(cache))
    mgr = mgr_for(tmp_path, [entry])
    mgr.download("m1")
    dest = cache / "weights.bin"
    dest.write_bytes(b"truncated")  # corrupt out-of-band
    assert mgr.local_available(mgr.get_entry("m1")) is False
