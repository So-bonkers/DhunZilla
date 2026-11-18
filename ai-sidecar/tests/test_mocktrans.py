"""mocktrans byte-level correctness (WP-0119).

The conformance suite verifies API shape; it happily passed a backend emitting *malformed* MIDI
(bytes built from unordered set literals — caught only by the opd parser in harness e2e). These
tests close that gap at unit level: structure assertions on build_midi output.
"""

from __future__ import annotations

import struct

from backends.mocktrans.impl import _PPQ, build_midi


def test_header_and_track_magic() -> None:
    data = build_midi(2.0)
    assert data[:4] == b"MThd"
    fmt, ntrks, ppq = struct.unpack(">HHH", data[8:14])  # MThd(4)+len(4), then HHH fields
    assert (fmt, ntrks, ppq) == (0, 1, _PPQ)
    assert data[14:18] == b"MTrk"


def test_event_order_is_canonical() -> None:
    """Tempo meta first; every note is delta-0 ON followed by +PPQ OFF — order is the bug class."""
    data = build_midi(1.0)  # 2 notes at 120 BPM
    body = data[22:]  # 14 header + MTrk magic(4) + track length(4)
    assert body[:7] == bytes([0x00, 0xFF, 0x51, 0x03]) + b"\x07\xa1\x20"  # 500000 µs = 120 BPM
    pos = 7
    for note in (60, 64):
        assert body[pos : pos + 4] == bytes([0x00, 0x90, note, 90])  # ON: delta 0
        pos += 4
        # OFF: delta PPQ=480 → two-byte VLQ 0x83 0x60
        assert body[pos : pos + 5] == bytes([0x83, 0x60, 0x80, note, 64])
        pos += 5
    assert body[pos:] == bytes([0x00, 0xFF, 0x2F, 0x00])


def test_duration_scales_note_count() -> None:
    # In this file's byte universe 0x90 occurs only as note-on status (vel=90=0x5A, deltas are
    # 0x00/0x83/0x60, notes 60/64/67/72, tempo payload 07 a1 20) — so a byte count is exact.
    assert build_midi(1.0).count(b"\x90") == 2  # quarters at 120 BPM
    assert build_midi(4.0).count(b"\x90") == 8
    assert build_midi(0.0).count(b"\x90") == 1  # never empty
