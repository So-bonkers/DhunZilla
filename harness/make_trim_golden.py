#!/usr/bin/env python3
"""make_trim_golden.py — generate tests/fixtures/goldens/kits/ramp48k.wav (WP-0156).

A deterministic integer ramp authored natively at 48000 Hz: mono PCM16, 4800 frames, frame i holds
the int16 value 5 * i (i = 0..4799). Every sample is an exact integer, so a sampler trim of this
asset can be checked sample-by-sample with no floating-point tolerance. Used by the `sampler_trim`
golden (docs/spec/project-model.md §Sampler trim — additive (WP-0156)). Pure stdlib, integer-only:
re-running reproduces the committed WAV byte-for-byte.

Usage:  python3 harness/make_trim_golden.py          # writes the kit asset
Recreate the golden render afterwards with:
    build/opd render tests/fixtures/goldens/sampler_trim.opj -o tests/fixtures/goldens/sampler_trim.wav
"""
import hashlib
import os
import struct
import sys

RATE = 48000            # native asset rate == project rate (no resampling involved)
N = 4800                # frames (0.1 s)
STEP = 5                # frame i holds STEP * i

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "fixtures", "goldens", "kits", "ramp48k.wav")


def ramp_pcm16(n: int, step: int) -> bytes:
    """Little-endian int16 samples, value = step * i (fits int16 for n * step <= 32767)."""
    vals = [step * i for i in range(n)]
    assert all(0 <= v <= 32767 for v in vals), "ramp must fit in PCM16"
    return struct.pack("<%dh" % n, *vals)


def write_wav_mono_pcm16(path: str, rate: int, pcm: bytes) -> None:
    data_len = len(pcm)
    hdr = b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", data_len)
    with open(path, "wb") as f:
        f.write(hdr + pcm)


def main() -> int:
    write_wav_mono_pcm16(OUT, RATE, ramp_pcm16(N, STEP))
    digest = hashlib.sha256(open(OUT, "rb").read()).hexdigest()
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {N} frames mono PCM16 @{RATE} Hz")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
