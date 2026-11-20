"""Stem bundle: the validated ``stems.json`` written beside every finished ``separate`` job.

Normative text: ``docs/spec/stem-bundle.md`` (WP-0174). One file tells the app/engine
everything it needs to import a split as aligned tracks — the shared format, and per stem
the file name, byte hash and levels. The job runner calls :func:`write_stem_bundle` when
finalizing a ``separate`` job; a contract violation (no stems, an unreadable WAV, stems of
differing shape) raises :class:`~ai_sidecar.base.InvalidRequest`, which the runner maps to
a failed job (``inference_failed``, ``retryable: false``).

Output is byte-stable for equal input: keys sorted, fixed indentation, no timestamps.

Usage example::

    from pathlib import Path
    from ai_sidecar.stem_bundle import write_stem_bundle

    job_dir = Path("out/jobs/abc123")
    bundle_path = write_stem_bundle(
        job_dir, {"bass": job_dir / "bass.wav", "vocals": job_dir / "vocals.wav"}
    )
    # -> job_dir/"stems.json"; stems sorted by name, all validated as one aligned set.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Any

import numpy as np

from ai_sidecar.base import InvalidRequest
from ai_sidecar.wav import read_wav

BUNDLE_FILENAME = "stems.json"
"""Name of the bundle inside the job output directory."""

BUNDLE_SCHEMA = 1
"""Schema version of the bundle document (bump on any incompatible layout change)."""

_CHUNK_BYTES = 1024 * 1024  # 1 MiB read chunks for hashing — bounded memory, few syscalls
_LEVEL_DECIMALS = 6


def _sha256(path: Path) -> str:
    """Return the sha256 hex digest of *path*'s bytes, streamed in 1 MiB chunks."""
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(_CHUNK_BYTES):
            digest.update(chunk)
    return digest.hexdigest()


def _levels(data: np.ndarray) -> tuple[float, float]:
    """``(peak, rms)`` of ``(frames, channels)`` audio, accumulated in float64.

    *peak* is ``max |sample|`` over all channels; *rms* is ``sqrt(mean(sample ** 2))``
    over all samples of all channels. Both are Python floats rounded to 6 decimals.
    """
    samples = np.asarray(data, dtype=np.float64)
    peak = float(np.max(np.abs(samples)))
    rms = float(np.sqrt(np.mean(np.square(samples))))
    return round(peak, _LEVEL_DECIMALS), round(rms, _LEVEL_DECIMALS)


def build_stem_bundle(artifacts: dict[str, Path]) -> dict[str, Any]:
    """Validate separation artifacts and build the bundle document (nothing written).

    *artifacts* maps stem name → WAV path, exactly as a ``separate`` backend returns them.
    Stems are read in name order; the first one defines the shape every other must match.

    Returns:
        A dict with exactly the keys ``schema, sample_rate, channels, frames, stems``, the
        ``stems`` list sorted by name with each entry holding exactly
        ``name, file, sha256, frames, peak, rms`` (see docs/spec/stem-bundle.md).

    Raises:
        InvalidRequest: no stems at all; a stem that is not a readable WAV; or a stem whose
            ``(sample_rate, channels, frames)`` differs from the first stem's.

    Usage example::

        bundle = build_stem_bundle({"bass": Path("bass.wav")})
        assert bundle["schema"] == 1 and bundle["stems"][0]["name"] == "bass"
    """
    if not artifacts:
        raise InvalidRequest("separate job produced no stems")

    stems: list[dict[str, Any]] = []
    shared: tuple[int, int, int] | None = None
    for name in sorted(artifacts):
        path = Path(artifacts[name])
        try:
            data, sample_rate = read_wav(path)
        except (OSError, ValueError) as exc:  # unreadable file or not a WAV we support
            raise InvalidRequest(f"stem '{name}' is not a readable WAV: {exc}") from exc

        shape = (int(sample_rate), int(data.shape[1]), int(data.shape[0]))
        if shared is None:
            shared = shape
        elif shape != shared:
            raise InvalidRequest(
                f"stem '{name}' has (sample_rate, channels, frames) = {shape}, expected {shared}"
            )

        peak, rms = _levels(data)
        stems.append(
            {
                "name": name,
                "file": path.name,
                "sha256": _sha256(path),
                "frames": int(data.shape[0]),
                "peak": peak,
                "rms": rms,
            }
        )

    assert shared is not None  # the non-empty check above guarantees this
    sample_rate, channels, frames = shared
    return {
        "schema": BUNDLE_SCHEMA,
        "sample_rate": sample_rate,
        "channels": channels,
        "frames": frames,
        "stems": stems,
    }


def write_stem_bundle(out_dir: Path, artifacts: dict[str, Path]) -> Path:
    """Build the bundle for *artifacts* and write it to ``<out_dir>/stems.json``.

    Written with ``json.dumps(indent=2, sort_keys=True)`` plus a trailing newline, so equal
    inputs give byte-identical files.

    Returns:
        The path of the file just written.

    Raises:
        InvalidRequest: as :func:`build_stem_bundle`.

    Usage example::

        path = write_stem_bundle(job_dir, {"drums": job_dir / "drums.wav"})
        assert path.name == BUNDLE_FILENAME and path.is_file()
    """
    bundle = build_stem_bundle(artifacts)
    bundle_path = out_dir / BUNDLE_FILENAME
    bundle_path.write_text(
        json.dumps(bundle, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return bundle_path
