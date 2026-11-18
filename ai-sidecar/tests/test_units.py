"""Unit tests: wav helper, base models/helpers, registry discovery edge cases."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from ai_sidecar.base import (
    Capabilities,
    HealthInfo,
    canonical_request_hash,
)
from ai_sidecar.registry import RegistryError, discover_backends
from ai_sidecar.wav import read_wav, write_wav

# ---------------------------------------------------------------- wav round-trip


def test_wav_roundtrip_float32(tmp_path: Path) -> None:
    rng = np.random.default_rng(0)
    data = (rng.random((1024, 2), dtype=np.float32) * 2.0 - 1.0).astype(np.float32)
    path = tmp_path / "rt.wav"
    write_wav(path, data, 44100)
    out, sr = read_wav(path)
    assert sr == 44100
    assert out.shape == (1024, 2)
    assert np.array_equal(out, data)


def test_wav_deterministic_bytes(tmp_path: Path) -> None:
    data = np.zeros((88, 2), dtype=np.float32)
    a, b = tmp_path / "a.wav", tmp_path / "b.wav"
    write_wav(a, data, 48000)
    write_wav(b, data, 48000)
    assert a.read_bytes() == b.read_bytes()


def test_wav_rejects_bad_input(tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        write_wav(tmp_path / "x.wav", np.zeros((0, 2), dtype=np.float32), 44100)
    with pytest.raises(ValueError):
        write_wav(tmp_path / "x.wav", np.zeros((8, 2), dtype=np.float32), 0)
    junk = tmp_path / "junk.wav"
    junk.write_bytes(b"not a wav at all")
    with pytest.raises(ValueError, match="RIFF"):
        read_wav(junk)


# ---------------------------------------------------------------- base models


def _caps(**kw) -> Capabilities:
    base = dict(vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"},
                max_duration_s=30.0)
    base.update(kw)
    return Capabilities(**base)


def test_capabilities_requires_all_tiers() -> None:
    with pytest.raises(ValueError, match="vram_tiers"):
        Capabilities(vram_tiers={"cpu": "ok"}, max_duration_s=10.0)
    with pytest.raises(ValueError):
        Capabilities(vram_tiers={"cpu": "maybe", "6gb": "ok", "12gb": "ok", "24gb": "ok"},
                     max_duration_s=10.0)
    assert _caps(text2loop=True).text2loop is True


def test_health_unavailable_requires_reason() -> None:
    tiers = {"cpu": "no", "6gb": "no", "12gb": "no", "24gb": "no"}
    with pytest.raises(ValueError, match="reason"):
        HealthInfo(status="unavailable", vram_tiers=tiers)
    ok = HealthInfo(status="unavailable", vram_tiers=tiers, reason="missing weights")
    assert ok.reason == "missing weights"


def test_canonical_request_hash_stable_and_order_insensitive() -> None:
    h1 = canonical_request_hash("mockgen", "generate", {"a": 1, "b": [1, 2], "c": "x"})
    h2 = canonical_request_hash("mockgen", "generate", {"c": "x", "b": [1, 2], "a": 1})
    h3 = canonical_request_hash("mockgen", "generate", {"a": 2, "b": [1, 2], "c": "x"})
    assert h1 == h2 and len(h1) == 64
    assert h1 != h3


# ---------------------------------------------------------------- registry


def test_registry_broken_adapters_are_skipped_not_fatal(tmp_path: Path) -> None:
    """A broken adapter package must not break discovery of the good ones."""
    pkg_root = tmp_path / "backends"
    pkg_root.mkdir()
    (pkg_root / "__init__.py").write_text("", encoding="utf-8")

    good = pkg_root / "good"
    good.mkdir()
    (good / "__init__.py").write_text(
        "from ai_sidecar.base import Capabilities, GenerationBackend, HealthInfo\n"
        "class GoodBackend(GenerationBackend):\n"
        "    id = 'good'\n"
        "    model_version = '0.1.0'\n"
        "    def capabilities(self):\n"
        "        return Capabilities(text2loop=True,"
        " vram_tiers={'cpu':'ok','6gb':'ok','12gb':'ok','24gb':'ok'},"
        " max_duration_s=10.0)\n"
        "    def _check_health(self):\n"
        "        return HealthInfo(status='ok',"
        " vram_tiers={'cpu':'ok','6gb':'ok','12gb':'ok','24gb':'ok'})\n"
        "    def generate(self, req):\n"
        "        raise NotImplementedError\n"
        "BACKEND_CLASS = GoodBackend\n",
        encoding="utf-8",
    )
    (pkg_root / "explodes").mkdir()
    (pkg_root / "explodes" / "__init__.py").write_text(
        "raise RuntimeError('boom')", encoding="utf-8"
    )
    (pkg_root / "noattr").mkdir()
    (pkg_root / "noattr" / "__init__.py").write_text("x = 1", encoding="utf-8")

    with pytest.warns(RuntimeWarning):
        found = discover_backends(pkg_root)
    assert set(found) == {"good"}


def test_registry_missing_dir_raises(tmp_path: Path) -> None:
    with pytest.raises(RegistryError, match="not found"):
        discover_backends(tmp_path / "nope")


def test_registry_duplicate_ids_skipped(tmp_path: Path) -> None:
    pkg_root = tmp_path / "backends"
    pkg_root.mkdir()
    (pkg_root / "__init__.py").write_text("", encoding="utf-8")
    snippet = (
        "from ai_sidecar.base import Capabilities, GenerationBackend, HealthInfo\n"
        "class Dup(GenerationBackend):\n"
        "    id = 'dup'\n"
        "    model_version = '0.1.0'\n"
        "    def capabilities(self):\n"
        "        return Capabilities(text2loop=True,"
        " vram_tiers={'cpu':'ok','6gb':'ok','12gb':'ok','24gb':'ok'},"
        " max_duration_s=10.0)\n"
        "    def _check_health(self):\n"
        "        return HealthInfo(status='ok',"
        " vram_tiers={'cpu':'ok','6gb':'ok','12gb':'ok','24gb':'ok'})\n"
        "    def generate(self, req):\n"
        "        raise NotImplementedError\n"
        "BACKEND_CLASS = Dup\n"
    )
    for name in ("a_first", "b_second"):
        d = pkg_root / name
        d.mkdir(parents=True)
        (d / "__init__.py").write_text(snippet, encoding="utf-8")
    with pytest.warns(RuntimeWarning, match="duplicate"):
        found = discover_backends(pkg_root)
    assert set(found) == {"dup"}


# ---------------------------------------------------------------- CI floor


def test_no_torch_in_core_env() -> None:
    """WP-0004 acceptance: the default env must not carry torch/ML libs."""
    import importlib.util

    for name in ("torch", "tensorflow", "onnxruntime", "demucs"):
        assert importlib.util.find_spec(name) is None, f"{name} leaked into the core env"


def test_provenance_json_shape_written_by_runner(tmp_path: Path) -> None:
    """Sanity: a done job's provenance.json parses and has exactly the 4 mandatory keys."""
    import asyncio

    from ai_sidecar.jobs import JobRunner

    registry = discover_backends()

    async def _run():
        runner = JobRunner(registry, jobs_dir=tmp_path, workers=1)
        await runner.start()
        try:
            jid = await runner.submit(
                "mockgen", "generate", {"prompt": "p", "duration_s": 0.5, "seed": 3}
            )
            return await runner.wait(jid, timeout_s=60)
        finally:
            await runner.stop()

    rec = asyncio.run(_run())
    assert rec.state == "done"
    prov = json.loads((rec.out_dir / "provenance.json").read_text())
    assert set(prov) == {"backend_id", "model_version", "seed", "request_hash"}
