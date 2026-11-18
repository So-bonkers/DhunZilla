#!/usr/bin/env python3
"""score_notes.py — transcription fidelity metrics for one (project, midi) pair.

Extracted verbatim from roundtrip_eval.sh's heredoc (WP-0127) so the corpus loop and the
single-case smoke share one scorer. Ground truth = the .opj's own notes (constant tempo);
detection = parsed from the transcribed SMF. Metrics:
  pitch F1 : multiset of pitches, timing-insensitive
  event F1 : pitch AND onset within TOL_S (greedy nearest)
Exit 0 iff both metrics clear their thresholds; prints one machine-readable line.

usage: score_notes.py <proj.opj> <transcribed.mid> <pitch_thr> <event_thr> <tol_s> [case_name]
"""
from __future__ import annotations

import json
import struct
import sys


def expected_events(opj_path: str) -> list[tuple[float, int]]:
    orig = json.load(open(opj_path))
    spb = 60.0 / float(orig["bpm"])
    return sorted(
        (n["t"] * spb, n["note"])
        for pat in orig["patterns"]
        for n in pat["notes"]
        if n.get("note", -1) >= 0
    )


def detected_events(mid_path: str) -> list[tuple[float, int]]:
    data = open(mid_path, "rb").read()
    assert data[:4] == b"MThd"
    hdrlen = struct.unpack(">I", data[4:8])[0]
    _fmt, ntrks, div = struct.unpack(">HHH", data[8:14])
    ppq = float(div)
    pos = 8 + hdrlen

    def vlq(p: int) -> tuple[int, int]:
        v = 0
        while True:
            c = data[p]
            p += 1
            v = (v << 7) | (c & 0x7F)
            if not c & 0x80:
                return v, p

    tempo_us = 500000.0  # SMF default
    got_ev: list[tuple[float, int]] = []
    for _ in range(ntrks):
        assert data[pos : pos + 4] == b"MTrk"
        ln = struct.unpack(">I", data[pos + 4 : pos + 8])[0]
        end = pos + 8 + ln
        pos += 8
        tick = 0
        running = 0
        open_notes: dict[int, int] = {}
        while pos < end:
            d, pos = vlq(pos)
            tick += d
            st = data[pos]
            if st < 0x80:
                st = running
            else:
                pos += 1
                running = st if st < 0xF0 else running
            if st == 0xFF:
                mt = data[pos]
                pos += 1
                ml, pos = vlq(pos)
                if mt == 0x51 and ml >= 3 and tempo_us == 500000.0:
                    tempo_us = float(int.from_bytes(data[pos : pos + 3], "big"))
                elif mt == 0x2F:
                    break
                pos += ml
            elif 0x90 <= st < 0xA0:
                key, vel = data[pos], data[pos + 1]
                pos += 2
                if vel > 0:
                    open_notes.setdefault(key, tick)
                elif key in open_notes:
                    got_ev.append((open_notes.pop(key) / ppq * tempo_us / 1e6, key))
            elif 0x80 <= st < 0x90:
                key = data[pos]
                pos += 2
                if key in open_notes:
                    got_ev.append((open_notes.pop(key) / ppq * tempo_us / 1e6, key))
            elif 0xC0 <= st < 0xE0:
                pos += 1
            else:
                pos += 2
    return sorted(got_ev)


def multiset_f1(want: list[int], got: list[int]) -> float:
    if not want or not got:
        return 0.0
    pool: dict[int, int] = {}
    for k in got:
        pool[k] = pool.get(k, 0) + 1
    hits = 0
    for k in want:
        if pool.get(k, 0) > 0:
            hits += 1
            pool[k] -= 1
    p, r = hits / len(got), hits / len(want)
    return 2 * p * r / (p + r) if p + r else 0.0


def event_f1(want_ev: list[tuple[float, int]], got_ev: list[tuple[float, int]], tol: float) -> float:
    remaining = list(got_ev)
    hits = 0
    for onset, key in sorted(want_ev):
        best, best_dt = None, tol
        for i, (g_onset, g_key) in enumerate(remaining):
            if g_key == key and abs(g_onset - onset) <= best_dt:
                best, best_dt = i, abs(g_onset - onset)
        if best is not None:
            hits += 1
            remaining.pop(best)
    p, r = (hits / len(got_ev) if got_ev else 0.0), (hits / len(want_ev) if want_ev else 0.0)
    return 2 * p * r / (p + r) if p + r else 0.0


def main() -> int:
    args = sys.argv[1:]
    if len(args) < 5:
        print(__doc__)
        return 2
    opj, mid = args[0], args[1]
    pitch_thr, event_thr, tol = float(args[2]), float(args[3]), float(args[4])
    case = args[5] if len(args) > 5 else "case"

    want_ev = expected_events(opj)
    got_ev = detected_events(mid)
    pf1 = multiset_f1(sorted(k for _, k in want_ev), sorted(k for _, k in got_ev))
    ef1 = event_f1(want_ev, got_ev, tol)
    ok = pf1 >= pitch_thr and ef1 >= event_thr
    print(f"{case}: notes={len(want_ev)} detected={len(got_ev)} "
          f"pitch_f1={pf1:.3f} event_f1={ef1:.3f} {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
