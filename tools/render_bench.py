#!/usr/bin/env python3
"""Render benchmark and determinism soak for opd (WP-0175).

Renders a project N times with a given ``opd`` binary, times each run, hashes
every output and prints one JSON report to stdout.  Exits non-zero when the
runs disagree, when a golden hash mismatches, or when the realtime factor is
below a floor.  This is dev tooling: stdlib only, wall-clock timing allowed.

CLI usage example::

    python3 tools/render_bench.py --opd build/opd \
        tests/fixtures/goldens/demo.opj --runs 5 --seed 7 --min-realtime 20
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import statistics
import struct
import subprocess
import sys
import tempfile
import time

EXIT_OK = 0
EXIT_NONDETERMINISTIC = 1
EXIT_USAGE = 2
EXIT_RENDER_FAILED = 3
EXIT_TOO_SLOW = 4


def wav_info(data: bytes) -> tuple[int, int, int]:
    """Return ``(sample_rate, channels, frames)`` for RIFF/WAVE bytes.

    Skips unknown chunks and honours the odd-size pad byte.  Unlike the stdlib
    ``wave`` module this accepts float WAVs.  Raises ``ValueError`` for
    non-WAV input or a missing/truncated ``fmt `` or ``data`` chunk.

    Example::

        >>> wav_info(open("out.wav", "rb").read())[:2]
        (48000, 2)
    """
    if len(data) < 12 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")
    fmt = None  # (channels, sample_rate, block_align) once the fmt chunk is seen
    data_len = None
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        (chunk_size,) = struct.unpack_from("<I", data, pos + 4)
        body = pos + 8
        if chunk_id == b"fmt ":
            if chunk_size < 16 or body + 16 > len(data):
                raise ValueError("truncated fmt chunk")
            _audio_format, channels, sample_rate, _byte_rate, block_align, \
                _bits = struct.unpack_from("<HHIIHH", data, body)
            if block_align <= 0:
                raise ValueError("bad block align in fmt chunk")
            fmt = (channels, sample_rate, block_align)
        elif chunk_id == b"data":
            if body + chunk_size > len(data):
                raise ValueError("data chunk extends past end of file")
            data_len = chunk_size
        pos = body + chunk_size + (chunk_size & 1)  # skip pad byte after odd chunks
    if fmt is None:
        raise ValueError("missing fmt chunk")
    if data_len is None:
        raise ValueError("missing data chunk")
    channels, sample_rate, block_align = fmt
    return sample_rate, channels, data_len // block_align


def _fmt_num(value: float | None) -> str:
    """Format a number for stderr messages; ``None`` prints as ``null``."""
    return "null" if value is None else f"{value:.3f}"


def main(argv: list[str] | None = None) -> int:
    """Entry point. Returns the process exit code.

    Example::

        >>> import sys; sys.exit(main(["--opd", "build/opd", "song.opj"]))
    """
    parser = argparse.ArgumentParser(
        prog="render_bench.py",
        description="Render a project N times with opd, time each run and "
                    "check that every output is byte-identical.")
    parser.add_argument("--opd", required=True, metavar="PATH",
                        help="path to the opd executable")
    parser.add_argument("project", metavar="PROJECT.opj",
                        help="project file to render")
    parser.add_argument("--runs", type=int, default=5, metavar="N",
                        help="number of renders (default: 5)")
    parser.add_argument("--seed", type=int, default=None, metavar="S",
                        help="render seed passed through to opd")
    parser.add_argument("--json", dest="json_out", default=None, metavar="OUT.json",
                        help="also write the JSON report to this file")
    parser.add_argument("--expect-sha256", dest="expect_sha256", default=None,
                        metavar="HEX", help="fail (exit 1) if run 0's sha256 differs")
    parser.add_argument("--min-realtime", dest="min_realtime", type=float,
                        default=None, metavar="F",
                        help="fail (exit 4) if realtime factor is below F")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.project):
        print(f"error: project file not found: {args.project}", file=sys.stderr)
        return EXIT_USAGE
    if not (os.path.isfile(args.opd) and os.access(args.opd, os.X_OK)):
        print(f"error: --opd is not an executable file: {args.opd}", file=sys.stderr)
        return EXIT_USAGE
    if args.runs < 1:
        print("error: --runs must be >= 1", file=sys.stderr)
        return EXIT_USAGE

    wall_seconds: list[float] = []
    hashes: list[str] = []
    first_bytes = 0
    sr = ch = frames = 0
    with tempfile.TemporaryDirectory(prefix="render_bench_") as tmpdir:
        for i in range(args.runs):
            out_path = os.path.join(tmpdir, f"r{i}.wav")
            cmd = [args.opd, "render", args.project, "-o", out_path]
            if args.seed is not None:
                cmd += ["--seed", str(args.seed)]
            t0 = time.perf_counter()
            proc = subprocess.run(cmd, capture_output=True)
            wall_seconds.append(time.perf_counter() - t0)
            if proc.returncode != 0:
                err = proc.stderr.decode("utf-8", "replace")[:300]
                print(f"error: render failed (exit {proc.returncode}): {err}",
                      file=sys.stderr)
                return EXIT_RENDER_FAILED
            try:
                with open(out_path, "rb") as f:
                    data = f.read()
                sr, ch, frames = wav_info(data)
            except (OSError, ValueError) as exc:
                print(f"error: {exc}", file=sys.stderr)
                return EXIT_RENDER_FAILED
            hashes.append(hashlib.sha256(data).hexdigest())
            if i == 0:
                first_bytes = len(data)

    audio_seconds = frames / sr
    min_s = min(wall_seconds)
    median_s = statistics.median(wall_seconds)
    max_s = max(wall_seconds)
    realtime_factor = None if median_s == 0 else audio_seconds / median_s
    distinct_outputs = len(set(hashes))

    report = {
        "project": args.project,
        "runs": args.runs,
        "seed": args.seed,
        "sample_rate": sr,
        "channels": ch,
        "frames": frames,
        "bytes": first_bytes,
        "audio_seconds": audio_seconds,
        "wall_seconds": wall_seconds,
        "min_s": min_s,
        "median_s": median_s,
        "max_s": max_s,
        "realtime_factor": realtime_factor,
        "sha256": hashes[0],
        "distinct_outputs": distinct_outputs,
        "deterministic": distinct_outputs == 1,
    }
    text = json.dumps(report, indent=2, sort_keys=True)
    print(text)
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            f.write(text + "\n")

    # Check order after the report is printed: 1 first, then 4.
    if distinct_outputs != 1:
        print(f"NONDETERMINISTIC: {distinct_outputs} distinct outputs in "
              f"{args.runs} runs", file=sys.stderr)
        return EXIT_NONDETERMINISTIC
    if args.expect_sha256 is not None and \
            hashes[0] != args.expect_sha256.lower():
        print(f"GOLDEN MISMATCH: expected {args.expect_sha256}, got {hashes[0]}",
              file=sys.stderr)
        return EXIT_NONDETERMINISTIC
    if args.min_realtime is not None and (
            realtime_factor is None or realtime_factor < args.min_realtime):
        print(f"TOO SLOW: realtime factor {_fmt_num(realtime_factor)} < "
              f"{args.min_realtime}", file=sys.stderr)
        return EXIT_TOO_SLOW
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
