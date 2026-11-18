"""demucs — real stem separation via Demucs htdemucs (WP-0006).

Cartridge pattern: the sidecar core never imports torch/demucs. This adapter shells
out to a *configured interpreter* that has demucs installed, keeping torch out of the
sidecar's own dependency tree entirely.

**Opt-in by design**: the module refuses to import unless ``OP_DEMUCS_PYTHON`` is set
to an executable python with ``demucs`` importable (e.g. a dedicated runtime venv).
The registry skips adapters that fail to import, so CI without torch simply doesn't
see this backend — and the conformance suite stays strict for everything else.

Setup on the reference rig (aarch64, CPU):
    export OP_DEMUCS_PYTHON=/path/to/demucs-venv/bin/python   # pip install demucs numpy

Weights: htdemucs (~80 MB) downloads from dl.fbaipublicfiles.com on first use into
the torch hub cache. Manifest entry (models/manifest.json) records the same source +
sha256; wiring the manifest download path into this adapter's expected layout is a
documented follow-up (see TASKS/WP-0006 handoff).
"""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from ai_sidecar.base import (
    Capabilities,
    HealthInfo,
    InvalidRequest,
    JobResult,
    SeparationBackend,
)

# ------------------------------------------------------------------ opt-in guard

_ENV_VAR = "OP_DEMUCS_PYTHON"
_INTERP = os.environ.get(_ENV_VAR, "").strip()
if not _INTERP:
    raise ImportError(
        f"demucs adapter disabled — set {_ENV_VAR} to a python interpreter with "
        "'demucs' installed (pip install demucs numpy) to enable it"
    )

SUPPORTED_MODELS: tuple[str, ...] = ("htdemucs",)
"""v0 supports the default hybrid-transformer model only; more = manifest work."""

DEFAULT_TIMEOUT_S = 1800.0


class DemucsBackend(SeparationBackend):
    """SeparationBackend driving ``python -m demucs.separate`` in a subprocess."""

    id = "demucs"
    model_version = "htdemucs-4.x"

    def __init__(self) -> None:
        self._health_info: HealthInfo | None = None  # verdict cached once per process

    # -- introspection -------------------------------------------------------------

    def capabilities(self) -> Capabilities:
        return Capabilities(
            stem_separation=True,
            deterministic=False,  # torch threading/BLAS variance — honest declaration
            vram_tiers={"cpu": "ok", "6gb": "ok", "12gb": "ok", "24gb": "ok"},
            max_duration_s=3600.0,
        )

    def _check_health(self) -> HealthInfo:
        """Validate the configured interpreter once per process; cache the verdict."""
        if self._health_info is not None:
            return self._health_info
        tiers = self.capabilities().vram_tiers
        interp = Path(_INTERP)
        if not interp.is_file() or not os.access(interp, os.X_OK):
            info = HealthInfo(
                status="unavailable", vram_tiers=tiers,
                reason=f"{_ENV_VAR}='{_INTERP}' is not an executable file",
            )
        else:
            try:
                proc = subprocess.run(
                    [str(interp), "-c", "import demucs; print(demucs.__version__)"],
                    capture_output=True, text=True, timeout=120.0,
                )
            except (OSError, subprocess.TimeoutExpired) as exc:
                info = HealthInfo(
                    status="unavailable", vram_tiers=tiers,
                    reason=f"cannot run {_ENV_VAR} interpreter: {exc!r}",
                )
            else:
                if proc.returncode == 0:
                    version = proc.stdout.strip() or "unknown"
                    self.model_version = f"htdemucs-demucs{version}"
                    info = HealthInfo(status="ok", vram_tiers=tiers)
                else:
                    tail = (proc.stderr or "").strip().splitlines()[-1:] or ["no stderr"]
                    info = HealthInfo(
                        status="unavailable", vram_tiers=tiers,
                        reason=(
                            f"demucs not importable in {_ENV_VAR} interpreter: {tail[0]} "
                            f"— run: {interp} -m pip install demucs numpy"
                        ),
                    )
        self._health_info = info
        return info

    # -- work ----------------------------------------------------------------------

    def separate(
        self,
        audio_path: Path,
        *,
        stems: list[str] | None = None,
        opts: dict[str, Any],
    ) -> JobResult:
        """Run htdemucs (4-stem) on *audio_path*; artifacts ``{stem: wav_path}``.

        Demucs always computes all four stems; ``stems`` selects which are copied
        into the job output dir. Outputs are 44.1 kHz stereo float32 WAVs.
        """
        self.report_progress(0.0)
        self.ensure_available(str(opts.get("hardware_hint", "cpu")))
        if not audio_path.is_file():
            raise InvalidRequest(
                f"demucs: input file not found: {audio_path} — pass a path the "
                "sidecar process can read"
            )
        model = str(opts.get("model", "htdemucs"))
        if model not in SUPPORTED_MODELS:
            raise InvalidRequest(
                f"demucs: unsupported model '{model}'; supported: {list(SUPPORTED_MODELS)}"
            )
        wanted = stems if stems else list(self.DEFAULT_STEMS)
        unknown = [s for s in wanted if s not in self.DEFAULT_STEMS]
        if unknown:
            raise InvalidRequest(
                f"demucs: unsupported stems {unknown}; supported: {list(self.DEFAULT_STEMS)}"
            )

        ctx = self.job_context()
        raw_root = ctx.output_dir / "separated"
        raw_root.mkdir(parents=True, exist_ok=True)
        cmd = [
            _INTERP, "-m", "demucs.separate",
            "-d", "cpu",  # CPU path always works (reference rig has no GPU in container)
            "-n", model,
            "-o", str(raw_root),
            str(audio_path),
        ]
        timeout = float(opts.get("timeout_s", DEFAULT_TIMEOUT_S))
        self.report_progress(0.5)  # indeterminate middle — spec allows sparse progress
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            raise InvalidRequest(
                f"demucs: separation exceeded {timeout}s — pass a larger opts.timeout_s "
                f"or shorten the input ({exc!r})"
            ) from exc
        if proc.returncode != 0:
            tail = (proc.stderr or "").strip().splitlines()[-3:] or ["no stderr"]
            raise InvalidRequest(
                "demucs: separation failed (exit "
                f"{proc.returncode}): {' | '.join(tail)}"
            )

        track_out = raw_root / model / audio_path.stem
        produced = sorted(p.name for p in track_out.glob("*.wav"))
        artifacts: dict[str, Path] = {}
        for i, stem in enumerate(wanted):
            src = track_out / f"{stem}.wav"
            if not src.is_file():
                raise InvalidRequest(
                    f"demucs: expected stem '{stem}' missing (produced: {produced}) — "
                    "model output layout changed?"
                )
            dst = ctx.output_dir / f"{stem}.wav"
            shutil.copyfile(src, dst)
            artifacts[stem] = dst
            self.report_progress(0.5 + 0.5 * (i + 1) / len(wanted))

        return JobResult(artifacts=artifacts, provenance=self.new_provenance(), raw_dir=raw_root)


BACKEND_CLASS = DemucsBackend
