"""basicpitch — audio→MIDI transcription via Spotify Basic Pitch on ONNX Runtime (WP-0008).

Cartridge pattern (identical philosophy to the demucs adapter): torch/TensorFlow stay
outside the sidecar core. **TensorFlow has no aarch64 wheels at all** (latest publish
x86_64/Windows only), so this backend runs Basic Pitch's shipped ONNX export through
``onnxruntime`` — first-class on both amd64 and arm64. See ADR-0005.

**Opt-in**: module refuses to import unless ``OP_TRANSCRIBE_PYTHON`` points at a python
with the runtime installed. Reference setup (py3.12, works on aarch64):

    uv venv .venv-transcribe --python 3.12
    VIRTUAL_ENV=.venv-transcribe uv pip install \\
        onnxruntime librosa mido midiutil numpy scipy soundfile \\
        pretty_midi mir_eval matplotlib resampy
    VIRTUAL_ENV=.venv-transcribe uv pip install --no-deps basic-pitch

Model weights ship inside the basic-pitch wheel (nmp.onnx); the manifest entry pins
the package version, not a separate download.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Any

from ai_sidecar.base import (
    Capabilities,
    HealthInfo,
    InvalidRequest,
    JobResult,
    TranscriptionBackend,
)

# ------------------------------------------------------------------ opt-in guard

_ENV_VAR = "OP_TRANSCRIBE_PYTHON"
_INTERP = os.environ.get(_ENV_VAR, "").strip()
if not _INTERP:
    raise ImportError(
        f"basicpitch adapter disabled — set {_ENV_VAR} to a python interpreter with "
        "'basic-pitch --no-deps + onnxruntime' installed (see this module docstring)"
    )

_WRAPPER = Path(__file__).resolve().parent / "run_transcribe.py"
DEFAULT_TIMEOUT_S = 600.0


class BasicPitchBackend(TranscriptionBackend):
    """TranscriptionBackend driving Basic Pitch (ONNX) in a subprocess."""

    id = "basicpitch"
    model_version = "basic-pitch-nmp-onnx"

    def __init__(self) -> None:
        self._health_info: HealthInfo | None = None

    # -- introspection -------------------------------------------------------------

    def capabilities(self) -> Capabilities:
        return Capabilities(
            transcription=True,
            deterministic=False,  # onnxruntime thread-scheduling variance — honest default
            vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"},
            max_duration_s=3600.0,
        )

    def _check_health(self) -> HealthInfo:
        if self._health_info is not None:
            return self._health_info
        tiers = self.capabilities().vram_tiers
        interp = Path(_INTERP)
        if not interp.is_file() or not os.access(interp, os.X_OK):
            info = HealthInfo(
                status="unavailable", vram_tiers=tiers,
                reason=f"{_ENV_VAR}='{_INTERP}' is not an executable file",
            )
        elif not _WRAPPER.is_file():
            info = HealthInfo(
                status="unavailable", vram_tiers=tiers,
                reason=f"cartridge wrapper missing at {_WRAPPER} — repo checkout broken?",
            )
        else:
            try:
                proc = subprocess.run(
                    [str(interp), "-c",
                     "import basic_pitch, onnxruntime; print(onnxruntime.__version__)"],
                    capture_output=True, text=True, timeout=120.0,
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                info = HealthInfo(
                    status="unavailable", vram_tiers=tiers,
                    reason=f"cannot run {_ENV_VAR} interpreter: {exc!r}",
                )
            else:
                if proc.returncode == 0:
                    self.model_version = f"basic-pitch-nmp-onnx-ort{proc.stdout.strip()}"
                    info = HealthInfo(status="ok", vram_tiers=tiers)
                else:
                    tail = (proc.stderr or "").strip().splitlines()[-1:] or ["no stderr"]
                    info = HealthInfo(
                        status="unavailable", vram_tiers=tiers,
                        reason=(
                            f"basic_pitch/onnxruntime not importable in {_ENV_VAR}: {tail[0]} "
                            "— see the setup block in backends/basicpitch/impl.py"
                        ),
                    )
        self._health_info = info
        return info

    # -- work ----------------------------------------------------------------------

    def transcribe(self, audio_path: Path, *, opts: dict[str, Any]) -> JobResult:
        """Transcribe *audio_path* to MIDI; artifacts ``{"midi": path}``.

        opts: ``min_hz``/``max_hz`` (float, both required together) forward Basic
        Pitch's note-frequency limits; ``timeout_s`` bounds the subprocess.
        """
        self.report_progress(0.0)
        self.ensure_available(str(opts.get("hardware_hint", "cpu")))
        if not audio_path.is_file():
            raise InvalidRequest(
                f"basicpitch: input file not found: {audio_path} — pass a path the "
                "sidecar process can read"
            )
        ctx = self.job_context()
        out_midi = ctx.output_dir / "transcription.mid"
        cmd = [str(_INTERP), str(_WRAPPER), str(audio_path), str(out_midi)]
        if opts.get("min_hz") is not None or opts.get("max_hz") is not None:
            try:
                lo, hi = float(opts["min_hz"]), float(opts["max_hz"])
            except (KeyError, TypeError, ValueError) as exc:
                raise InvalidRequest(
                    "basicpitch: opts.min_hz and opts.max_hz must be given together as numbers"
                ) from exc
            if not (0.0 < lo < hi):
                raise InvalidRequest("basicpitch: need 0 < min_hz < max_hz")
            cmd += [str(lo), str(hi)]
        timeout = float(opts.get("timeout_s", DEFAULT_TIMEOUT_S))
        self.report_progress(0.2)
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            raise InvalidRequest(
                f"basicpitch: transcription exceeded {timeout}s — pass a larger opts.timeout_s"
                f" or shorten the input ({exc!r})"
            ) from exc
        if proc.returncode != 0 or not out_midi.is_file():
            tail = (proc.stderr or "").strip().splitlines()[-3:] or ["no stderr"]
            raise InvalidRequest(
                f"basicpitch: transcription failed (exit {proc.returncode}): {' | '.join(tail)}"
            )
        self.report_progress(1.0)
        return JobResult(artifacts={"midi": out_midi}, provenance=self.new_provenance())


BACKEND_CLASS = BasicPitchBackend
