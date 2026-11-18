#!/usr/bin/env python3
"""make_resampled_golden.py — generate tests/fixtures/goldens/kits/kick44k.wav (WP-0138).

A deterministic synth kick authored natively at 44100 Hz, used by the `resampled` golden to
prove the sampler resamples non-native-rate assets at load (docs/spec/project-model.md
§Sampler resampling — additive (WP-0138)). Pure stdlib; every operation is a fixed sequence of IEEE-754
double ops with no randomness or wall-clock input, so re-running reproduces the committed WAV
byte-for-byte on this machine.

Usage:  python3 harness/make_resampled_golden.py          # writes the kit asset
Recreate the full golden set afterwards with:
    build/opd render tests/fixtures/goldens/resampled.opj -o tests/fixtures/goldens/resampled.wav
"""
import hashlib
import math
import os
import struct
import sys

RATE = 44100            # native asset rate — deliberately NOT the project rate (48000)
DUR_S = 0.35            # spec: kit sample <= 0.5 s
N = int(RATE * DUR_S)   # 15434 frames, mono (int() truncates 15434.99…)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "fixtures", "goldens", "kits", "kick44k.wav")


def synth_kick(n: int, rate: int) -> list:
    """Pitch-swept sine body + phase-locked second partial, exponential decay, 1 ms attack.

    All math in doubles with a fixed operation order (phase accumulated by recurrence), so the
    sample values are reproducible bit-for-bit on any IEEE-754 machine running the same libm.
    """
    f0, f1, tau_f = 160.0, 42.0, 0.028   # pitch sweep 160 Hz -> ~42 Hz
    tau_a, atk_s = 0.11, 0.001           # amplitude decay tau, attack length
    phase = 0.0
    out = []
    for i in range(n):
        t = float(i) / float(rate)
        f = f1 + (f0 - f1) * math.exp(-t / tau_f)
        phase += 2.0 * math.pi * f / float(rate)
        env = math.exp(-t / tau_a)
        atk = 1.0 if t >= atk_s else t / atk_s
        s = (math.sin(phase) + 0.35 * math.sin(2.0 * phase)) * env * atk
        out.append(s)
    return out


def to_pcm16(samples: list, target_peak_dbfs: float = -1.0) -> bytes:
    """Normalize to target peak and quantize with round-to-nearest (banker's rounding, stable)."""
    peak = max(abs(s) for s in samples)
    scale = (10.0 ** (target_peak_dbfs / 20.0)) / peak
    vals = []
    for s in samples:
        v = int(round(s * scale * 32767.0))
        if v > 32767:
            v = 32767
        elif v < -32768:
            v = -32768
        vals.append(v)
    return struct.pack("<%dh" % len(vals), *vals)


def write_wav_mono_pcm16(path: str, rate: int, pcm: bytes) -> None:
    data_len = len(pcm)
    hdr = b"RIFF" + struct.pack("<I", 36 + data_len) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", data_len)
    with open(path, "wb") as f:
        f.write(hdr + pcm)


def main() -> int:
    pcm = to_pcm16(synth_kick(N, RATE))
    write_wav_mono_pcm16(OUT, RATE, pcm)
    digest = hashlib.sha256(open(OUT, "rb").read()).hexdigest()
    print(f"wrote {os.path.relpath(OUT, ROOT)}: {N} frames mono PCM16 @{RATE} Hz")
    print(f"sha256 {digest}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
