#!/usr/bin/env python3
"""Bundled generated content: CC0 48 kHz mono drum kit + synth preset patches (WP-0215).

Synthesises every byte of the bundled starter content in-repo: nine PCM16 mono 48 kHz WAV
one-shots under ``<out>/kits``, four ``set_synth_params`` presets under ``<out>/presets`` and a
``manifest.json`` of lowercase-hex sha256 sums at ``<out>/manifest.json``.  Standard library
only, fixed seed, no wall clock, no environment or cwd dependence: two runs with the same
seed into different directories are byte-identical (ADR-0004), which is what makes the
committed ``assets/`` tree verifiable by regeneration (``tests/test_bundled_assets.cpp``).

Every kit file is canonical PCM WAV (44-byte header, ``fmt`` tag 1, 1 channel, 16 bit,
48000 Hz, no extra chunks), frame count ``round(sec * 48000)`` per the WP-0215 table, with a
48-frame onset ramp (frame 0 is written as the exact integer 0), a 240-frame fade-out to
silence (last frame is the exact integer 0) — both ramps applied BEFORE normalisation — and
a final peak normalisation to 0.70 linear (-3.10 dBFS), comfortably below -1 dBFS.

CLI usage example::

    python3 tools/gen_bundled_kit.py                     # writes <repo>/assets
    python3 tools/gen_bundled_kit.py --out /tmp/x --seed 1
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import struct
import sys

EXIT_OK = 0
EXIT_USAGE = 2

SAMPLE_RATE = 48000
DEFAULT_SEED = 20240517
FADE_IN_FRAMES = 48        # onset ramp: multiplier i / 48  (frame 0 -> exactly 0)
FADE_OUT_FRAMES = 240      # fade-out ramp: (n - 1 - i) / 240  (last frame -> exactly 0)
PEAK_TARGET = 0.70         # linear peak after normalisation = -3.10 dBFS (< -1 dBFS)
GENERATOR = "tools/gen_bundled_kit.py"
LICENSE = "CC0-1.0"
LICENSE_NOTE = "generated in-repo by tools/gen_bundled_kit.py; no third-party samples"

TWO_PI = 2.0 * math.pi


# --------------------------------------------------------------------------- DSP helpers

def _swept_sine(n: int, f_start: float, f_end: float, tau_f: float, tau_a: float) -> list[float]:
    """Pitch-swept sine with exponential frequency approach f_end + (f_start - f_end)·e^(-t/tau_f)
    and exponential amplitude decay e^(-t/tau_a).  Phase is the closed-form integral of the
    sweep, so no per-sample phase accumulation error.  Boring and explicit (style rule)."""
    out = []
    for i in range(n):
        t = i / SAMPLE_RATE
        e = math.exp(-t / tau_f)
        phase = TWO_PI * (f_end * t + (f_start - f_end) * tau_f * (1.0 - e))
        out.append(math.sin(phase) * math.exp(-t / tau_a))
    return out


def _sine(n: int, freq: float, tau: float) -> list[float]:
    """Plain sine at `freq` Hz with exponential decay e^(-t/tau)."""
    return [math.sin(TWO_PI * freq * (i / SAMPLE_RATE)) * math.exp(-(i / SAMPLE_RATE) / tau)
            for i in range(n)]


def _noise(rng: random.Random, n: int, tau: float, offset: int = 0) -> list[float]:
    """White noise (from the caller's seeded rng) enveloped by e^(-(t - t_off)/tau), silent
    before the optional frame `offset`."""
    out = [0.0] * n
    for i in range(offset, n):
        out[i] = rng.uniform(-1.0, 1.0) * math.exp(-((i - offset) / SAMPLE_RATE) / tau)
    return out


def _mix(*voices: list[float]) -> list[float]:
    """Element-wise sum of equal-length voices."""
    out = list(voices[0])
    for v in voices[1:]:
        for i, x in enumerate(v):
            out[i] += x
    return out


def _biquad(samples: list[float], b0: float, b1: float, b2: float,
            a0: float, a1: float, a2: float) -> list[float]:
    """Direct Form I biquad (RBJ cookbook coefficients in the callers), zero initial state.
    Deterministic left-to-right pass."""
    out = []
    x1 = x2 = y1 = y2 = 0.0
    b0, b1, b2, a1, a2 = b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0
    for x in samples:
        y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        out.append(y)
        x2, x1, y2, y1 = x1, x, y1, y
    return out


def _highpass(samples: list[float], freq: float, q: float) -> list[float]:
    w0 = TWO_PI * freq / SAMPLE_RATE
    alpha = math.sin(w0) / (2.0 * q)
    cos_w0 = math.cos(w0)
    return _biquad(samples, (1.0 + cos_w0) / 2.0, -(1.0 + cos_w0), (1.0 + cos_w0) / 2.0,
                   1.0 + alpha, -2.0 * cos_w0, 1.0 - alpha)


def _bandpass(samples: list[float], freq: float, q: float) -> list[float]:
    w0 = TWO_PI * freq / SAMPLE_RATE
    alpha = math.sin(w0) / (2.0 * q)
    return _biquad(samples, alpha, 0.0, -alpha,
                   1.0 + alpha, -2.0 * math.cos(w0), 1.0 - alpha)


# --------------------------------------------------------------------------- the nine sounds
# Each takes (n frames, seeded rng) and returns raw float samples; the caller applies the
# onset/fade ramps and normalisation.  DSP is free per the WP-0215 spec; properties are fixed.

def _kick(n: int, rng: random.Random) -> list[float]:
    """Pitch-dropping sine body (~150 -> 46 Hz) plus a short click."""
    body = _swept_sine(n, 150.0, 46.0, 0.055, 0.150)
    click = _noise(rng, n, 0.002)
    return _mix(body, [0.45 * x for x in click])


def _snare(n: int, rng: random.Random) -> list[float]:
    """Two-part tonal body plus band-passed noise."""
    body = _mix([0.6 * x for x in _sine(n, 185.0, 0.09)],
                [0.4 * x for x in _sine(n, 210.0, 0.085)])
    nz = _bandpass(_noise(rng, n, 0.07), 1800.0, 0.8)
    return _mix(body, nz)


def _clap(n: int, rng: random.Random) -> list[float]:
    """Three staggered noise bursts (first one loudest) plus a longer tail burst."""
    voices = []
    for offset, gain, tau in ((0, 1.0, 0.006), (240, 0.85, 0.006), (432, 0.7, 0.006),
                              (800, 0.45, 0.060)):
        nz = _bandpass(_noise(rng, n, tau, offset=offset), 1150.0, 1.2)
        voices.append([gain * x for x in nz])
    return _mix(*voices)


def _hat(n: int, rng: random.Random, tau_noise: float, tau_ring: float) -> list[float]:
    """Bright high-passed noise plus a metallic inharmonic ring (shared by both hats)."""
    nz = _highpass(_noise(rng, n, tau_noise), 7500.0, 0.707)
    ring = _mix([0.10 * x for x in _sine(n, 10400.0, tau_ring)],
                [0.08 * x for x in _sine(n, 13100.0, tau_ring)],
                [0.06 * x for x in _sine(n, 15900.0, tau_ring)])
    return _mix(nz, ring)


def _hat_closed(n: int, rng: random.Random) -> list[float]:
    return _hat(n, rng, 0.012, 0.020)


def _hat_open(n: int, rng: random.Random) -> list[float]:
    return _hat(n, rng, 0.120, 0.200)


def _rim(n: int, rng: random.Random) -> list[float]:
    """Very short click plus two high partials."""
    click = _bandpass(_noise(rng, n, 0.002), 2100.0, 4.0)
    partials = _mix([0.7 * x for x in _sine(n, 3400.0, 0.030)],
                    [0.5 * x for x in _sine(n, 4700.0, 0.025)])
    return _mix(click, partials)


def _tom(n: int, rng: random.Random) -> list[float]:
    """Pitch-dropping sine body with a long decay."""
    body = _swept_sine(n, 220.0, 85.0, 0.120, 0.280)
    click = _noise(rng, n, 0.003)
    return _mix(body, [0.25 * x for x in click])


def _cowbell(n: int, rng: random.Random) -> list[float]:
    """Two inharmonic partials (classic ~1:1.9 bell)."""
    return _mix(_sine(n, 560.0, 0.12), [0.7 * x for x in _sine(n, 1064.0, 0.09)])


def _clave(n: int, rng: random.Random) -> list[float]:
    """Short band-passed click."""
    return _bandpass(_noise(rng, n, 0.03), 2600.0, 2.0)


# (file name, length in seconds, synthesiser) — the WP-0215 table, in file order.
KIT_SPECS = (
    ("kick.wav", 0.45, _kick),
    ("snare.wav", 0.35, _snare),
    ("clap.wav", 0.40, _clap),
    ("hat_closed.wav", 0.08, _hat_closed),
    ("hat_open.wav", 0.50, _hat_open),
    ("rim.wav", 0.12, _rim),
    ("tom.wav", 0.55, _tom),
    ("perc_cowbell.wav", 0.30, _cowbell),
    ("perc_clave.wav", 0.10, _clave),
)


# --------------------------------------------------------------------------- presets
# Each `args` object is exactly the argument set of the existing set_synth_params operation
# (docs/spec/operations.md §Synth parameters), all values inside the ranges of
# docs/spec/project-model.md §Synth voice v1, so {"op": "set_synth_params", "index": t, **args}
# is accepted by op::apply_op unchanged.

PRESETS = [
    {"name": "bass", "op": "set_synth_params",
     "args": {"adsr": {"attack_s": 0.004, "decay_s": 0.14, "release_s": 0.08, "sustain": 0.65},
              "cutoff_hz": 620.0, "resonance": 4.0, "waveform": "saw"}},
    {"name": "lead", "op": "set_synth_params",
     "args": {"adsr": {"attack_s": 0.002, "decay_s": 0.2, "release_s": 0.25, "sustain": 0.7},
              "cutoff_hz": 4200.0, "resonance": 2.0, "waveform": "square"}},
    {"name": "pad", "op": "set_synth_params",
     "args": {"adsr": {"attack_s": 0.6, "decay_s": 0.8, "release_s": 1.2, "sustain": 0.8},
              "cutoff_hz": 1400.0, "resonance": 0.9, "waveform": "saw"}},
    {"name": "pluck", "op": "set_synth_params",
     "args": {"adsr": {"attack_s": 0.001, "decay_s": 0.12, "release_s": 0.05, "sustain": 0.0},
              "cutoff_hz": 2600.0, "resonance": 1.2, "waveform": "sine"}},
]


# --------------------------------------------------------------------------- file assembly

def _wav_bytes(int16_samples: list[int]) -> bytes:
    """Canonical mono PCM16 48 kHz WAV: 44-byte header, no extra chunks, little-endian."""
    data = struct.pack("<%dh" % len(int16_samples), *int16_samples)
    return (b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16)
            + b"data" + struct.pack("<I", len(data)) + data)


def _ramp_and_normalise(samples: list[float]) -> list[int]:
    """Onset ramp (first 48 frames, multiplier i/48) and fade-out (last 240 frames,
    multiplier (n-1-i)/240) BEFORE normalisation, then scale so the maximum |x| is exactly
    PEAK_TARGET, then quantise to signed 16 bit round-to-nearest.  Frame 0 and the last
    frame become the exact integer 0."""
    n = len(samples)
    out = list(samples)
    for i in range(min(FADE_IN_FRAMES, n)):
        out[i] *= i / FADE_IN_FRAMES
    for i in range(max(0, n - FADE_OUT_FRAMES), n):
        out[i] *= (n - 1 - i) / FADE_OUT_FRAMES
    peak = max(abs(x) for x in out)
    if peak > 0.0:
        scale = PEAK_TARGET / peak
        out = [x * scale for x in out]
    pcm = []
    for x in out:
        v = int(math.floor(x * 32768.0 + 0.5))  # round to nearest
        pcm.append(max(-32768, min(32767, v)))  # saturation guard; never hit at 0.70 peak
    return pcm


def generate(out_dir: str, seed: int) -> int:
    """Write the 11 output files under `out_dir`; returns the number of files written."""
    kits_dir = os.path.join(out_dir, "kits")
    presets_dir = os.path.join(out_dir, "presets")
    os.makedirs(kits_dir, exist_ok=True)
    os.makedirs(presets_dir, exist_ok=True)

    rng = random.Random(seed)  # one seeded stream, sounds generated in fixed order
    files: dict[str, bytes] = {}
    for name, seconds, synth in KIT_SPECS:
        n = int(round(seconds * SAMPLE_RATE))
        raw = synth(n, rng)
        wav = _wav_bytes(_ramp_and_normalise(raw))
        rel = "kits/" + name
        with open(os.path.join(kits_dir, name), "wb") as f:
            f.write(wav)
        files[rel] = wav

    presets_doc = {
        "format_version": 0,
        "generator": GENERATOR,
        "license": LICENSE,
        "note": LICENSE_NOTE,
        "presets": PRESETS,
    }
    presets_bytes = (json.dumps(presets_doc, indent=2, sort_keys=True) + "\n").encode("utf-8")
    with open(os.path.join(presets_dir, "synth_presets.json"), "wb") as f:
        f.write(presets_bytes)
    files["presets/synth_presets.json"] = presets_bytes

    manifest = {
        "format_version": 0,
        "generator": GENERATOR,
        "seed": seed,
        "sample_rate": SAMPLE_RATE,
        "license": LICENSE,
        "note": LICENSE_NOTE,
        "files": {rel: hashlib.sha256(data).hexdigest()
                  for rel, data in sorted(files.items())},
    }
    with open(os.path.join(out_dir, "manifest.json"), "wb") as f:
        f.write((json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"))

    return len(files) + 1  # the 10 hashed files plus manifest.json itself


def main(argv: list[str] | None = None) -> int:
    """Entry point. Returns the process exit code.

    Example::

        >>> import sys; sys.exit(main(["--out", "/tmp/assets", "--seed", "1"]))
    """
    parser = argparse.ArgumentParser(
        prog="gen_bundled_kit.py",
        description="Synthesise the bundled CC0 drum kit and synth presets "
                    "(stdlib only, fixed seed, byte-deterministic).")
    default_out = os.path.normpath(
        os.path.join(os.path.dirname(os.path.realpath(__file__)), os.pardir, "assets"))
    parser.add_argument("--out", default=default_out, metavar="DIR",
                        help="output root (default: the repo's assets/ directory)")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, metavar="N",
                        help="generator seed (default: %d)" % DEFAULT_SEED)
    args = parser.parse_args(argv)

    count = generate(args.out, args.seed)
    print("gen_bundled_kit: wrote %d files to %s (seed %d)"
          % (count, args.out, args.seed))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
