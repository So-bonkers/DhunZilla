"""Robustness tests for ai_sidecar.wav.read_wav — mirror of the C++ WP-0125 fuzz classes.

The sidecar reads user/bundle-supplied WAVs; every crafted-invalid header must raise a clean
ValueError (jobs map it to failed), never ZeroDivisionError/struct.error/uncaught crash.
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np
import pytest

from ai_sidecar.wav import read_wav, write_wav


def _seed_bytes(tmp_path: Path) -> bytes:
    p = tmp_path / "seed.wav"
    write_wav(p, np.zeros((64, 2), dtype=np.float32), 48000)
    return p.read_bytes()


def _patch(raw: bytes, off: int, fmt: str, val: int) -> bytes:
    return raw[:off] + struct.pack(fmt, val) + raw[off + struct.calcsize(fmt):]


# fmt chunk starts at 12; fields: tag@20 ch@22 sr@24 (bytes 28-31 skipped) bits@34.
@pytest.mark.parametrize(
    ("off", "fmt", "val"),
    [
        (34, "<H", 16),     # float tag with PCM-16 bit depth → unsupported-format error
        (34, "<H", 0),      # bits=0 → block_align would be 0 → ZeroDivisionError pre-fix
        (34, "<H", 24),     # 24-bit: rejected domain-wise before any decode math
        (34, "<H", 8),
        (22, "<H", 0),      # channels=0
        (22, "<H", 7),      # multichannel: outside the mono/stereo contract
        (24, "<I", 0),      # sample rate 0
        (24, "<I", 2**31),  # implausible sample rate
    ],
)
def test_crafted_headers_reject_cleanly(tmp_path: Path, off: int, fmt: str, val: int) -> None:
    raw = _seed_bytes(tmp_path)
    p = tmp_path / "bad.wav"
    p.write_bytes(_patch(raw, off, fmt, val))
    with pytest.raises(ValueError):
        read_wav(p)


@pytest.mark.parametrize("cut", [0, 1, 11, 12, 36, 43, 50])
def test_truncations_reject_cleanly(tmp_path: Path, cut: int) -> None:
    raw = _seed_bytes(tmp_path)[:cut]
    p = tmp_path / "trunc.wav"
    p.write_bytes(raw)
    with pytest.raises(ValueError):
        read_wav(p)


@pytest.fixture
def seeded_rng() -> np.random.Generator:
    return np.random.default_rng(20261120)


def test_random_mutations_never_escape(tmp_path: Path, seeded_rng: np.random.Generator) -> None:
    raw = bytearray(_seed_bytes(tmp_path))
    p = tmp_path / "mut.wav"
    for _ in range(500):
        m = bytearray(raw)
        n_edits = int(seeded_rng.integers(1, 6))
        for _ in range(n_edits):
            pos = int(seeded_rng.integers(0, len(m)))
            if seeded_rng.random() < 0.3:
                del m[pos]
            else:
                m[pos] = int(seeded_rng.integers(0, 256))
        p.write_bytes(bytes(m))
        try:
            out, sr = read_wav(p)
        except ValueError:
            continue  # the only acceptable failure mode
        assert np.all(np.isfinite(out)), "accepted file must contain finite samples"
        assert 0 < sr <= 1_000_000
