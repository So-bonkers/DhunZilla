#!/usr/bin/env python3
"""make_repitch_golden.py — generate tests/fixtures/goldens/kits/saw48k.wav (WP-0167).

A deterministic integer sawtooth authored natively at 48000 Hz: mono PCM16, 9600 frames, frame i
holds the int16 value (i % 100) * 200 - 9900 — a 480 Hz sawtooth whose every sample is an exact
integer. With the `sampler_repitch` golden (voice root_note 60, docs/spec/project-model.md
§Sampler repitch — additive (WP-0167)) the +12-semitone note then plays out[n] == x[2n] exactly, so
the render can be checked sample-by-sample with no floating-point tolerance. Pure stdlib,
integer-only: re-running reproduces the committed WAV byte-for-byte.

Usage:  python3 harness/make_repitch_golden.py          # writes the kit asset
Recreate the golden render afterwards with:
    build/opd render tests/fixtures/goldens/sampler_repitch.opj -o tests/fixtures/goldens/sampler_repitch.wav
"""
import hashlib
import os
import struct
import sys

RATE = 48000            # native asset rate == project rate (no resampling involved)
N = 9600                # frames (0.2 s)


def saw_pcm16(n: int) -> bytes:
    """Little-endian int16 samples, value = (i % 100) * 200 - 9900 (range -9900..9900)."""
    vals = [(i % 100) * 200 - 9900 for i in range(n)]
    assert all(-32768 <= v <= 32767 for v in vals), "sawtooth must fit in PCM16"
    return struct.pack("<%dh" % n, *vals)


def write_wav_mono_pcm16(path: str, rate: int, pcm: bytes) -> None:
    data_len = len(pcm)
    hdr = b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", data_len)
    with open(path, "wb") as f:
        f.write(hdr + pcm)


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(root, "tests", "fixtures", "goldens", "kits", "saw48k.wav")
    write_wav_mono_pcm16(out, RATE, saw_pcm16(N))
    digest = hashlib.sha256(open(out, "rb").read()).hexdigest()
    print(f"wrote {os.path.relpath(out, root)}: {N} frames mono PCM16 @{RATE} Hz")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
