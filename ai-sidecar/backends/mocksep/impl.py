"""mocksep — real 4-band FFT stem splitter (docs/spec/backend-adapters.md).

Fake musically, real as DSP: overlap-free raised-cosine crossover masks over the
rFFT of the signal split the spectrum into bass (<150 Hz), drums (150–1 kHz),
vocals (1 kHz–8 kHz) and other (>8 kHz). Deterministic numpy only — no torch.

Output: 44.1 kHz stereo float32 WAVs, one per stem, in the job output dir.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np

from ai_sidecar.base import (
    Capabilities,
    HealthInfo,
    InvalidRequest,
    JobResult,
    SeparationBackend,
)

TARGET_SR = 44100
CROSSOVERS_HZ = (150.0, 1000.0, 8000.0)
_OCTAVE_RATIO = 2.0**0.25  # quarter-octave raised-cosine transition band


def _resample_linear(x: np.ndarray, sr_in: int, sr_out: int) -> np.ndarray:
    """Deterministic linear-interpolation resampling of ``(frames, channels)``."""
    if sr_in == sr_out:
        return x
    frames_in = x.shape[0]
    frames_out = max(1, int(round(frames_in * sr_out / sr_in)))
    pos = np.linspace(0.0, frames_in - 1, frames_out, dtype=np.float64)
    low = np.floor(pos).astype(np.int64)
    high = np.minimum(low + 1, frames_in - 1)
    frac = (pos - low).astype(np.float32)[:, None]
    return (x[low] * (1.0 - frac) + x[high] * frac).astype(np.float32)


def _band_masks(freqs: np.ndarray) -> dict[str, np.ndarray]:
    """Overlap-free raised-cosine masks per stem over positive rFFT frequencies."""
    low_parts: list[np.ndarray] = []
    high_parts: list[np.ndarray] = []
    for fc in CROSSOVERS_HZ:
        t = np.clip(np.log2(np.maximum(freqs, 1e-9) / fc) / np.log2(_OCTAVE_RATIO), -1.0, 1.0)
        angle = (t + 1.0) * 0.25 * np.pi  # 0 → fully low, π/2 → fully high
        low_parts.append(np.cos(angle) ** 2)
        high_parts.append(np.sin(angle) ** 2)

    one = np.ones_like(freqs)
    masks: dict[str, np.ndarray] = {}
    masks["bass"] = low_parts[0]
    masks["drums"] = high_parts[0] * low_parts[1]
    masks["vocals"] = high_parts[0] * high_parts[1] * low_parts[2]
    masks["other"] = high_parts[0] * high_parts[1] * high_parts[2]
    assert one.shape == masks["bass"].shape
    return masks


class MockSepBackend(SeparationBackend):
    """SeparationBackend doing a real 4-band FFT split (deterministic)."""

    id = "mocksep"
    model_version = "mocksep-0.1.0"

    def capabilities(self) -> Capabilities:
        return Capabilities(
            stem_separation=True,
            deterministic=True,
            vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"},
            max_duration_s=3600.0,
        )

    def _check_health(self) -> HealthInfo:
        return HealthInfo(
            status="ok", vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"}
        )

    def separate(
        self,
        audio_path: Path,
        *,
        stems: list[str] | None = None,
        opts: dict[str, Any],
    ) -> JobResult:
        """Split *audio_path* into stem WAVs; unknown stem names raise InvalidRequest."""
        self.report_progress(0.0)
        self.ensure_available(str(opts.get("hardware_hint", "cpu")))
        if not audio_path.is_file():
            raise InvalidRequest(
                f"mocksep: input file not found: {audio_path} — pass a path the sidecar "
                "process can read"
            )
        wanted = stems if stems else list(self.DEFAULT_STEMS)
        unknown = [s for s in wanted if s not in self.DEFAULT_STEMS]
        if unknown:
            raise InvalidRequest(
                f"mocksep: unsupported stems {unknown}; supported: {list(self.DEFAULT_STEMS)}"
            )

        from ai_sidecar.wav import read_wav, write_wav

        ctx = self.job_context()
        x, sr_in = read_wav(audio_path)
        if x.shape[1] == 1:
            x = np.repeat(x, 2, axis=1)
        x = _resample_linear(x, sr_in, TARGET_SR)

        self.report_progress(0.3)
        self.check_cancelled()  # last checkpoint before any stem file is written
        spectrum = np.fft.rfft(x, axis=0)
        freqs = np.fft.rfftfreq(x.shape[0], d=1.0 / TARGET_SR)
        masks = _band_masks(freqs.astype(np.float64))

        artifacts: dict[str, Path] = {}
        for i, stem in enumerate(wanted):
            band = np.fft.irfft(spectrum * masks[stem][:, None], n=x.shape[0], axis=0)
            band = np.clip(band, -1.0, 1.0).astype(np.float32)
            path = ctx.output_dir / f"{stem}.wav"
            write_wav(path, band, TARGET_SR)
            artifacts[stem] = path
            self.report_progress(0.3 + 0.7 * (i + 1) / len(wanted))
            self.check_cancelled()

        return JobResult(artifacts=artifacts, provenance=self.new_provenance())
