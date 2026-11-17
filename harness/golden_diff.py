#!/usr/bin/env python3
"""golden_diff.py — compare two canonical PCM16 WAVs. Spec: docs/spec/render-harness.md.

Usage: golden_diff.py A.wav B.wav [--mode bitexact|dbfs] [--tol FLOAT]   (tol in normalized amplitude)
Prints one machine-parsable RESULT line; exit 0 pass / 1 fail / 2 usage-error.
Pure stdlib so it runs on any CI runner instantly.
"""
import argparse
import math
import struct
import sys


def read_pcm16_stereo(path):
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 44 or data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not canonical RIFF/WAVE")
    fmt_off = data.find(b"fmt ", 12, 64)
    if fmt_off < 0:
        raise ValueError(f"{path}: missing fmt chunk")
    tag, chans, rate = struct.unpack_from("<HHI", data, fmt_off + 8)
    bits = struct.unpack_from("<H", data, fmt_off + 22)[0]
    if tag != 1 or chans != 2 or bits != 16:
        raise ValueError(f"{path}: expected PCM16 stereo, got tag={tag} ch={chans} bits={bits}")
    doff = data.find(b"data", fmt_off)
    if doff < 0:
        raise ValueError(f"{path}: missing data chunk")
    nbytes = struct.unpack_from("<I", data, doff + 4)[0]
    payload = data[doff + 8: doff + 8 + nbytes]
    return rate, payload


def rms_db(payload):
    n = len(payload) // 2
    if n == 0:
        return -120.0
    samples = struct.unpack(f"<{n}h", payload[: n * 2])
    mean_sq = sum(s * s for s in samples) / n
    if mean_sq == 0:
        return -120.0
    return 20.0 * math.log10(math.sqrt(mean_sq) / 32768.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--mode", choices=["bitexact", "dbfs"], default="bitexact")
    ap.add_argument("--tol", type=float, default=1e-7)
    args = ap.parse_args()

    try:
        _, pa = read_pcm16_stereo(args.a)
        _, pb = read_pcm16_stereo(args.b)
    except (ValueError, OSError) as e:
        print(f"RESULT fail mode={args.mode} error={e}")
        sys.exit(2)

    if args.mode == "bitexact":
        ok = pa == pb and len(pa) == len(pb)
        print(f"RESULT {'pass' if ok else 'fail'} mode=bitexact bytes_a={len(pa)} bytes_b={len(pb)}")
        sys.exit(0 if ok else 1)

    n = min(len(pa), len(pb)) // 2
    sa = struct.unpack(f"<{n}h", pa[: n * 2])
    sb = struct.unpack(f"<{n}h", pb[: n * 2])
    max_abs = max((abs(x - y) for x, y in zip(sa, sb)), default=0) / 32768.0
    len_ok = len(pa) == len(pb)
    ok = len_ok and max_abs <= args.tol
    print(
        f"RESULT {'pass' if ok else 'fail'} mode=dbfs max_abs_delta={max_abs:.9f} "
        f"rms_db_a={rms_db(pa):.2f} rms_db_b={rms_db(pb):.2f} len_match={len_ok}"
    )
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
