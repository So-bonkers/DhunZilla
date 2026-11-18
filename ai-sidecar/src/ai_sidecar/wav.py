"""Minimal float32 WAV reader/writer using only stdlib + numpy.

The stdlib ``wave`` module cannot handle IEEE float PCM, and we deliberately avoid a
soundfile dependency in the core env (WP-0004: no heavy ML libs). Only the formats
OpenPattern itself produces/needs are supported: 32-bit float PCM (writer + reader)
and 16-bit int PCM (reader only, converted to float32).

Usage example::

    import numpy as np
    from ai_sidecar.wav import write_wav, read_wav

    samples = np.zeros((44100, 2), dtype=np.float32)   # 1 s stereo
    write_wav(Path("out.wav"), samples, sample_rate=44100)
    data, sr = read_wav(Path("out.wav"))
    assert data.shape == (44100, 2) and sr == 44100
"""

from __future__ import annotations

import struct
from pathlib import Path

import numpy as np

_FMT_FLOAT32 = 3  # WAVE_FORMAT_IEEE_FLOAT
_FMT_PCM16 = 1  # WAVE_FORMAT_PCM


def write_wav(path: Path, samples: np.ndarray, sample_rate: int) -> None:
    """Write interleaved float32 PCM WAV bytes deterministically (no timestamps).

    *samples* is ``(frames, channels)`` float32; mono may be passed as ``(frames,)``.
    Output is byte-identical for identical input arrays on the same machine.

    Raises:
        ValueError: on empty/oversized/mis-shaped input or bad sample_rate.
    """
    if sample_rate <= 0:
        raise ValueError(f"sample_rate must be positive, got {sample_rate}")
    data = np.asarray(samples)
    if data.ndim == 1:
        data = data.reshape(-1, 1)
    if data.ndim != 2:
        raise ValueError(f"samples must be (frames,) or (frames, channels), got ndim={data.ndim}")
    frames, channels = data.shape
    if frames == 0:
        raise ValueError("samples has zero frames")
    if frames > 2**31 - 1:
        raise ValueError("too many frames for a WAV file")
    data = np.ascontiguousarray(data, dtype=np.float32)

    bits_per_sample = 32
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8
    data_size = frames * block_align
    header = b"RIFF" + struct.pack("<I", 36 + data_size) + b"WAVE"
    fmt = (
        b"fmt "
        + struct.pack(
            "<IHHIIHH",
            16,
            _FMT_FLOAT32,
            channels,
            sample_rate,
            byte_rate,
            block_align,
            bits_per_sample,
        )
    )
    path.write_bytes(header + fmt + b"data" + struct.pack("<I", data_size) + data.tobytes())


def read_wav(path: Path) -> tuple[np.ndarray, int]:
    """Read a WAV file → ``(float32 array (frames, channels), sample_rate)``.

    Supports 32-bit float and 16-bit int PCM; other formats raise ValueError.
    """
    raw = path.read_bytes()
    if len(raw) < 12 or raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a RIFF/WAVE file")

    fmt_chunk: bytes | None = None
    data_bytes: bytes | None = None
    pos = 12
    while pos + 8 <= len(raw):
        chunk_id = raw[pos : pos + 4]
        (chunk_size,) = struct.unpack("<I", raw[pos + 4 : pos + 8])
        body_start = pos + 8
        body_end = min(body_start + chunk_size, len(raw))  # tolerate truncated last chunk
        if chunk_id == b"fmt ":
            fmt_chunk = raw[body_start:body_end]
        elif chunk_id == b"data":
            data_bytes = raw[body_start:body_end]
        pos = body_end + (chunk_size % 2)  # chunks are word-aligned
    if fmt_chunk is None or len(fmt_chunk) < 16:
        raise ValueError(f"{path}: missing/short fmt chunk")
    if data_bytes is None:
        raise ValueError(f"{path}: missing data chunk")

    (audio_format, channels, sample_rate, _byte_rate, _block_align, bits) = struct.unpack(
        "<HHIIHH", fmt_chunk[:16]
    )
    if channels <= 0 or sample_rate <= 0:
        raise ValueError(f"{path}: bad header (channels={channels}, rate={sample_rate})")
    block_align = channels * bits // 8
    usable = len(data_bytes) - (len(data_bytes) % block_align)
    if usable == 0:
        raise ValueError(f"{path}: empty data chunk")

    if audio_format == _FMT_FLOAT32 and bits == 32:
        frames = usable // block_align
        out = np.frombuffer(data_bytes[:usable], dtype="<f4").reshape(frames, channels)
    elif audio_format == _FMT_PCM16 and bits == 16:
        frames = usable // block_align
        out = (
            np.frombuffer(data_bytes[:usable], dtype="<i2").reshape(frames, channels).astype(
                np.float32
            )
            / np.float32(32768.0)
        )
    else:
        raise ValueError(f"{path}: unsupported format (code={audio_format}, bits={bits})")
    return np.ascontiguousarray(out, dtype=np.float32), int(sample_rate)
