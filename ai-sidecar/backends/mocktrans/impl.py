"""mocktrans — deterministic fake transcriber for CI (docs/spec/backend-adapters.md).

No ML: writes a small SMF whose notes cycle C4/E4/G4/C5 in quarter notes over the input's
duration. The bytes are real format-0 MIDI that our own ``opd midi2opj`` parses — so the whole
hum→MIDI→project loop is testable in CI without any transcription runtime installed.
"""

from __future__ import annotations

import struct
from pathlib import Path
from typing import Any

from ai_sidecar.base import (
    Capabilities,
    HealthInfo,
    InvalidRequest,
    JobResult,
    TranscriptionBackend,
)

_PPQ = 480
_NOTES = (60, 64, 67, 72)


def _vlq(n: int) -> bytes:
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def build_midi(duration_s: float) -> bytes:
    """Format-0 SMF, 120 BPM quarter notes cycling ``_NOTES`` to cover *duration_s*."""
    n_notes = max(1, int(round(duration_s * 2.0)))  # quarters at 120 BPM
    track = bytearray()
    track += _vlq(0) + bytes([0xFF, 0x51, 0x03]) + struct.pack(">I", 500_000)[-3:]  # 120 BPM
    for i in range(n_notes):
        note = _NOTES[i % len(_NOTES)]
        track += _vlq(0) + bytes([0x90, note, 90])
        track += _vlq(_PPQ) + bytes([0x80, note, 64])
    track += _vlq(0) + bytes([0xFF, 0x2F, 0x00])
    header = b"MThd" + struct.pack(">IHHH", 6, 0, 1, _PPQ)
    return header + b"MTrk" + struct.pack(">I", len(track)) + bytes(track)


class MockTransBackend(TranscriptionBackend):
    """TranscriptionBackend emitting a deterministic canned MIDI (CI fixture in backend form)."""

    id = "mocktrans"
    model_version = "mocktrans-0.1.0"

    def capabilities(self) -> Capabilities:
        return Capabilities(
            transcription=True,
            deterministic=True,  # genuinely: byte-identical output for identical input length
            vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"},
            max_duration_s=3600.0,
        )

    def _check_health(self) -> HealthInfo:
        return HealthInfo(
            status="ok", vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"}
        )

    def transcribe(self, audio_path: Path, *, opts: dict[str, Any]) -> JobResult:
        """Write a canned MIDI sized to *audio_path*'s duration; artifact key ``"midi"``."""
        self.report_progress(0.0)
        self.ensure_available(str(opts.get("hardware_hint", "cpu")))
        if not audio_path.is_file():
            raise InvalidRequest(
                f"mocktrans: input file not found: {audio_path} — pass a path the sidecar "
                "process can read"
            )
        from ai_sidecar.wav import read_wav

        self.report_progress(0.3)
        x, sr = read_wav(audio_path)
        duration_s = x.shape[0] / float(sr)
        out_midi = self.job_context().output_dir / "transcription.mid"
        out_midi.write_bytes(build_midi(duration_s))
        self.report_progress(1.0)
        return JobResult(artifacts={"midi": out_midi}, provenance=self.new_provenance())


BACKEND_CLASS = MockTransBackend
