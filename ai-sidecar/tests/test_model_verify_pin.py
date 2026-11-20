"""WP-0173 offline verify/pin tests: report statuses (missing / size_mismatch /
hash_mismatch / ok), atomic manifest rewrite with everything else left equal,
--from-dir, awaiting-weights refusal, and CLI exit codes + stderr lines.

Every file lives under tmp_path; the URL host is ``weights.invalid`` (RFC 2606) so any
accidental network use would fail loudly instead of reaching the internet."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
from typing import Any

import pytest
from conftest import write_manifest

from ai_sidecar.cli import main as cli_main
from ai_sidecar.model_manager import (
    CHUNK_SIZE,
    ModelAwaitingWeights,
    ModelManager,
    ModelNotFound,
    WeightsMissing,
    sha256_file,
)

ALPHA = b"alpha weights payload " * 1024  # ~22 KB
BETA = b"beta weights payload " * 700


def sha256_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def source(name: str, *, sha256: str, size_bytes: int | None = None) -> dict[str, Any]:
    """One manifest source against an unreachable .invalid host."""
    return {
        "url": f"https://weights.invalid/{name}",
        "sha256": sha256,
        "size_bytes": size_bytes,
        "hash_verified": False,
    }


def make_entry(
    sources: list[dict[str, Any]],
    *,
    model_id: str = "m1",
    cache_dir: str,
    status: str = "live",
    capabilities_hint: dict[str, Any] | None = None,
) -> dict[str, Any]:
    return {
        "id": model_id,
        "backend": "demo",
        "task": "stem_separation",
        "sources": sources,
        "cache_dir": cache_dir,
        "license": {
            "code": "MIT",
            "weights_tier": "personal-use-ok",
            "weights_spdx": "NOASSERTION",
            "notes": "test fixture license notes",
            "verified_on": "2026-11-18",
        },
        "default_enabled": False,
        "status": status,
        "capabilities_hint": capabilities_hint or {},
    }


def two_source_entry(cache_dir: str) -> dict[str, Any]:
    """alpha.bin + beta.bin in that order, both pinned to all-zero placeholder digests."""
    return make_entry(
        [source("alpha.bin", sha256="0" * 64), source("beta.bin", sha256="0" * 64)],
        cache_dir=cache_dir,
    )


def mgr_for(tmp_path: Path, entries: list[dict[str, Any]]) -> tuple[ModelManager, Path]:
    manifest_path = write_manifest(tmp_path / "models" / "manifest.json", entries)
    return ModelManager(manifest_path), manifest_path


# ---------------------------------------------------------------- sha256_file


def test_sha256_file_matches_hashlib_across_chunk_boundary(tmp_path: Path) -> None:
    """Streaming digest equals a one-shot hashlib digest, incl. multi-chunk files."""
    blob = (bytes(range(256)) * 256) * 20 + b"tail"  # >1 MiB → several CHUNK_SIZE reads
    assert len(blob) > CHUNK_SIZE
    path = tmp_path / "blob.bin"
    path.write_bytes(blob)
    assert sha256_file(path) == sha256_of(blob)


# ---------------------------------------------------------------- verify


def test_verify_ok_hashes_every_source_in_order(tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)
    (cache / "beta.bin").write_bytes(BETA)
    entry = two_source_entry(str(cache))
    entry["sources"][0].update(sha256=sha256_of(ALPHA), size_bytes=len(ALPHA))
    entry["sources"][1]["sha256"] = sha256_of(BETA)  # size not declared → hash decides
    mgr, _ = mgr_for(tmp_path, [entry])

    out = mgr.verify("m1")
    assert out["id"] == "m1"
    assert out["ok"] is True
    assert [f["file"] for f in out["files"]] == ["alpha.bin", "beta.bin"]
    assert [f["status"] for f in out["files"]] == ["ok", "ok"]
    assert out["files"][0]["expected_sha256"] == sha256_of(ALPHA)
    assert out["files"][0]["actual_sha256"] == sha256_of(ALPHA)
    assert out["files"][1]["actual_sha256"] == sha256_of(BETA)


def test_verify_all_missing_reports_every_source(tmp_path: Path) -> None:
    """The failed-acceptance probe: nothing on disk → still a full report, never an error."""
    cache = tmp_path / "cache"  # deliberately never created
    mgr, _ = mgr_for(tmp_path, [two_source_entry(str(cache))])

    out = mgr.verify("m1")
    assert out["ok"] is False
    assert [f["file"] for f in out["files"]] == ["alpha.bin", "beta.bin"]  # manifest order
    assert [f["status"] for f in out["files"]] == ["missing", "missing"]
    assert all(f["actual_sha256"] is None for f in out["files"])
    assert out["files"][0]["expected_sha256"] == "0" * 64


def test_verify_hash_mismatch_reports_actual_digest(tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)
    (cache / "beta.bin").write_bytes(b"corrupted delivery, right size " * 234)
    entry = two_source_entry(str(cache))
    entry["sources"][0].update(sha256=sha256_of(ALPHA), size_bytes=len(ALPHA))
    wrong = cache / "beta.bin"
    entry["sources"][1]["size_bytes"] = wrong.stat().st_size  # size passes, hash must not
    mgr, _ = mgr_for(tmp_path, [entry])

    out = mgr.verify("m1")
    assert out["ok"] is False
    assert out["files"][0]["status"] == "ok"
    assert out["files"][1]["status"] == "hash_mismatch"
    assert out["files"][1]["actual_sha256"] == sha256_of(wrong.read_bytes())


def test_verify_size_mismatch_skips_hashing(tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA[:100])  # truncated out-of-band
    (cache / "beta.bin").write_bytes(BETA)
    entry = two_source_entry(str(cache))
    entry["sources"][0].update(sha256=sha256_of(ALPHA), size_bytes=len(ALPHA))
    entry["sources"][1].update(sha256=sha256_of(BETA), size_bytes=len(BETA))
    mgr, _ = mgr_for(tmp_path, [entry])

    out = mgr.verify("m1")
    assert out["ok"] is False
    assert out["files"][0]["status"] == "size_mismatch"
    assert out["files"][0]["actual_sha256"] is None  # not hashed on size mismatch
    assert out["files"][1]["status"] == "ok"


def test_verify_unknown_model_raises(tmp_path: Path) -> None:
    mgr, _ = mgr_for(tmp_path, [two_source_entry(str(tmp_path / "cache"))])
    with pytest.raises(ModelNotFound, match="m1"):
        mgr.verify("nope")


def test_awaiting_weights_refused_by_verify_and_pin(tmp_path: Path) -> None:
    entry = make_entry(
        [],
        cache_dir=str(tmp_path / "cache"),
        status="awaiting-weights",
        capabilities_hint={
            "unverified_claims": True,
            "announcement_url": "https://example.com/release-notes",
            "announced_on": "2026-11-18",
        },
    )
    mgr, _ = mgr_for(tmp_path, [entry])
    with pytest.raises(ModelAwaitingWeights, match="nothing to verify"):
        mgr.verify("m1")
    with pytest.raises(ModelAwaitingWeights, match="nothing to pin"):
        mgr.pin("m1")


# ---------------------------------------------------------------- pin


def test_pin_writes_digest_size_verified_and_leaves_rest_equal(tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)
    (cache / "beta.bin").write_bytes(BETA)
    entry = two_source_entry(str(cache))
    mgr, manifest_path = mgr_for(tmp_path, [entry])

    mgr.pin("m1")

    expected = {"schema": 1, "models": [copy.deepcopy(entry)]}
    expected["models"][0]["sources"][0].update(
        sha256=sha256_of(ALPHA), size_bytes=len(ALPHA), hash_verified=True
    )
    expected["models"][0]["sources"][1].update(
        sha256=sha256_of(BETA), size_bytes=len(BETA), hash_verified=True
    )
    # Exact bytes: only those three fields per source changed; indent 2 + trailing newline.
    assert manifest_path.read_text(encoding="utf-8") == json.dumps(expected, indent=2) + "\n"
    # The manifest is still valid and now verifies clean against itself.
    assert mgr.verify("m1")["ok"] is True


def test_pin_result_shape(tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)
    (cache / "beta.bin").write_bytes(BETA)
    mgr, _ = mgr_for(tmp_path, [two_source_entry(str(cache))])

    out = mgr.pin("m1")
    assert set(out.keys()) == {"id", "pinned"}
    assert out["id"] == "m1"
    assert [set(p.keys()) for p in out["pinned"]] == [{"file", "sha256", "size_bytes"}] * 2
    assert [p["file"] for p in out["pinned"]] == ["alpha.bin", "beta.bin"]  # manifest order
    assert out["pinned"][0] == {"file": "alpha.bin", "sha256": sha256_of(ALPHA),
                                "size_bytes": len(ALPHA)}
    assert out["pinned"][1] == {"file": "beta.bin", "sha256": sha256_of(BETA),
                                "size_bytes": len(BETA)}


def test_pin_missing_file_raises_and_manifest_untouched(tmp_path: Path) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)  # beta.bin never delivered
    mgr, manifest_path = mgr_for(tmp_path, [two_source_entry(str(cache))])
    before = manifest_path.read_bytes()

    with pytest.raises(WeightsMissing, match="beta.bin") as excinfo:
        mgr.pin("m1")
    assert excinfo.value.error_code == "weights_missing"
    assert manifest_path.read_bytes() == before  # byte-identical, not even reformatted
    assert not (manifest_path.parent / "manifest.json.tmp").exists()


def test_pin_from_dir_does_not_touch_cache(tmp_path: Path) -> None:
    delivery = tmp_path / "delivery"
    delivery.mkdir()
    (delivery / "alpha.bin").write_bytes(ALPHA)
    (delivery / "beta.bin").write_bytes(BETA)
    cache = tmp_path / "cache"
    cache.mkdir()
    mgr, manifest_path = mgr_for(tmp_path, [two_source_entry(str(cache))])

    out = mgr.pin("m1", from_dir=delivery)
    assert out["pinned"][0]["sha256"] == sha256_of(ALPHA)
    assert list(cache.iterdir()) == []  # cache stayed empty; delivery untouched too
    raw = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert raw["models"][0]["sources"][0]["sha256"] == sha256_of(ALPHA)
    assert raw["models"][0]["sources"][0]["hash_verified"] is True


# ---------------------------------------------------------------- CLI


def test_cli_verify_ok_exit_0(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)
    (cache / "beta.bin").write_bytes(BETA)
    entry = two_source_entry(str(cache))
    entry["sources"][0]["sha256"] = sha256_of(ALPHA)
    entry["sources"][1]["sha256"] = sha256_of(BETA)
    _, manifest_path = mgr_for(tmp_path, [entry])

    rc = cli_main(["models", "verify", "m1", "--manifest", str(manifest_path)])
    captured = capsys.readouterr()
    assert rc == 0 and captured.err == ""
    assert json.loads(captured.out) == {
        "id": "m1",
        "ok": True,
        "files": [
            {"file": "alpha.bin", "status": "ok", "expected_sha256": sha256_of(ALPHA),
             "actual_sha256": sha256_of(ALPHA)},
            {"file": "beta.bin", "status": "ok", "expected_sha256": sha256_of(BETA),
             "actual_sha256": sha256_of(BETA)},
        ],
    }


def test_cli_verify_all_missing_prints_report_exit_1(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """The failed-acceptance probe: full JSON report on stdout AND exit 1."""
    _, manifest_path = mgr_for(tmp_path, [two_source_entry(str(tmp_path / "cache"))])

    rc = cli_main(["models", "verify", "m1", "--manifest", str(manifest_path)])
    captured = capsys.readouterr()
    assert rc == 1 and captured.err == ""
    report = json.loads(captured.out)
    assert report["ok"] is False
    assert [f["file"] for f in report["files"]] == ["alpha.bin", "beta.bin"]
    assert [f["status"] for f in report["files"]] == ["missing", "missing"]
    assert all(f["actual_sha256"] is None for f in report["files"])


def test_cli_pin_prints_dict_exit_0(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)
    (cache / "beta.bin").write_bytes(BETA)
    _, manifest_path = mgr_for(tmp_path, [two_source_entry(str(cache))])

    rc = cli_main(["models", "pin", "m1", "--manifest", str(manifest_path)])
    captured = capsys.readouterr()
    assert rc == 0 and captured.err == ""
    assert json.loads(captured.out) == {
        "id": "m1",
        "pinned": [
            {"file": "alpha.bin", "sha256": sha256_of(ALPHA), "size_bytes": len(ALPHA)},
            {"file": "beta.bin", "sha256": sha256_of(BETA), "size_bytes": len(BETA)},
        ],
    }


def test_cli_pin_from_dir_flag(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    delivery = tmp_path / "delivery"
    delivery.mkdir()
    (delivery / "alpha.bin").write_bytes(ALPHA)
    (delivery / "beta.bin").write_bytes(BETA)
    _, manifest_path = mgr_for(tmp_path, [two_source_entry(str(tmp_path / "cache"))])

    rc = cli_main(
        ["models", "pin", "m1", "--manifest", str(manifest_path), "--from-dir", str(delivery)]
    )
    captured = capsys.readouterr()
    assert rc == 0 and captured.err == ""
    assert json.loads(captured.out)["pinned"][1]["sha256"] == sha256_of(BETA)


def test_cli_model_manager_error_line_on_stderr(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """Unknown id and missing weights both print `error: <code>: …` and nothing on stdout."""
    cache = tmp_path / "cache"
    cache.mkdir()
    (cache / "alpha.bin").write_bytes(ALPHA)  # beta.bin never delivered
    _, manifest_path = mgr_for(tmp_path, [two_source_entry(str(cache))])

    rc = cli_main(["models", "verify", "nope", "--manifest", str(manifest_path)])
    captured = capsys.readouterr()
    assert rc == 1 and captured.out == ""
    assert captured.err.startswith("error: unknown_model: ")

    before = manifest_path.read_bytes()
    rc = cli_main(["models", "pin", "m1", "--manifest", str(manifest_path)])
    captured = capsys.readouterr()
    assert rc == 1 and captured.out == ""
    assert captured.err.startswith("error: weights_missing: ")
    assert manifest_path.read_bytes() == before


def test_cli_bare_models_exits_2(capsys: pytest.CaptureFixture[str]) -> None:
    """`models_command` is required, so bare `opsidecar models` is an argparse error."""
    with pytest.raises(SystemExit) as excinfo:
        cli_main(["models"])
    assert excinfo.value.code == 2
