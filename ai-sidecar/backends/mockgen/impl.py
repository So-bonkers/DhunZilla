"""mockgen — deterministic seeded sine-chord "songs" (docs/spec/backend-adapters.md).

Zero heavy deps: numpy only. Declares ``text2loop`` only, ``deterministic=True``.
Same request + same seed → byte-identical WAV, guaranteed run-to-run on one machine
(``numpy.random.default_rng(seed)`` streams are stable; all math is float32).

Signal recipe (fake but exercises the whole pipeline):
  seed → scale (major/minor) + root note + 4-chord progression → per-segment triads of
  pure sines with a short linear attack/release envelope → stereo float32 @ 44.1 kHz.
"""

from __future__ import annotations

import numpy as np

from ai_sidecar.base import (
    Capabilities,
    GenerateRequest,
    GenerationBackend,
    HealthInfo,
    InvalidRequest,
    JobResult,
)

SAMPLE_RATE = 44100
MAJOR = (0, 2, 4, 5, 7, 9, 11)
MINOR = (0, 2, 3, 5, 7, 8, 10)
_ENVELOPE_S = 0.005  # attack/release per chord segment, avoids clicks


def _midi_to_hz(midi: float) -> float:
    return 440.0 * (2.0 ** ((midi - 69.0) / 12.0))


class MockGenBackend(GenerationBackend):
    """GenerationBackend producing seeded sine-chord loops."""

    id = "mockgen"
    model_version = "mockgen-0.1.0"

    def capabilities(self) -> Capabilities:
        return Capabilities(
            text2loop=True,
            deterministic=True,
            vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"},
            max_duration_s=600.0,
        )

    def _check_health(self) -> HealthInfo:
        # numpy is importable (we are loaded), so we are always healthy on CPU tiers.
        return HealthInfo(
            status="ok", vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"}
        )

    def generate(self, req: GenerateRequest) -> JobResult:
        """Render a deterministic sine-chord loop to ``audio.wav`` in the job dir."""
        self.report_progress(0.0)
        self.ensure_available(req.hardware_hint or "cpu")
        caps = self.capabilities()
        if req.duration_s > caps.max_duration_s:
            raise InvalidRequest(
                f"mockgen: duration_s={req.duration_s} exceeds max_duration_s="
                f"{caps.max_duration_s}; request a shorter loop"
            )

        ctx = self.job_context()
        rng = np.random.default_rng(req.seed)
        scale = MAJOR if rng.random() < 0.5 else MINOR
        root_midi = int(rng.integers(45, 61))
        degrees = [int(d) for d in rng.integers(0, 7, size=4)]

        frames = int(round(req.duration_s * SAMPLE_RATE))
        seg_len = max(1, frames // len(degrees))
        out = np.zeros((frames,), dtype=np.float32)
        env_n = max(1, int(_ENVELOPE_S * SAMPLE_RATE))

        self.report_progress(0.3)
        for i, degree in enumerate(degrees):
            start = i * seg_len
            stop = frames if i == len(degrees) - 1 else (i + 1) * seg_len
            length = stop - start
            if length <= 0:
                continue
            # Triad on the scale degree (root, third, fifth), one octave per voice.
            semis = [
                scale[degree],
                scale[(degree + 2) % 7] + (12 if degree + 2 >= 7 else 0),
                scale[(degree + 4) % 7] + (12 if degree + 4 >= 7 else 0),
            ]
            t = np.arange(length, dtype=np.float32) / SAMPLE_RATE
            seg = np.zeros(length, dtype=np.float32)
            for semi in semis:
                freq = _midi_to_hz(root_midi + semi)
                phase = float(rng.uniform(0.0, 2.0 * np.pi))
                seg += np.sin(2.0 * np.pi * freq * t + phase).astype(np.float32)
            seg *= 0.5 / 3.0  # keep peak amplitude well below clipping
            ramp = np.ones(length, dtype=np.float32)
            n = min(env_n, length // 2)
            if n > 1:
                ramp[:n] = np.linspace(0.0, 1.0, n, dtype=np.float32)
                ramp[-n:] = np.linspace(1.0, 0.0, n, dtype=np.float32)
            out[start:stop] = seg * ramp

        self.report_progress(0.8)
        stereo = np.repeat(out[:, None], 2, axis=1)  # (frames, 2) identical L/R
        from ai_sidecar.wav import write_wav

        audio_path = ctx.output_dir / "audio.wav"
        write_wav(audio_path, stereo, SAMPLE_RATE)
        self.report_progress(1.0)
        return JobResult(artifacts={"audio": audio_path}, provenance=self.new_provenance(req.seed))
