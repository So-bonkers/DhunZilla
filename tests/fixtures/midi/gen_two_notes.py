#!/usr/bin/env python3
"""Generate tests/fixtures/midi/two_notes.mid — hand-built SMF bytes, no dependencies.

Format 1, ppq=480, tempo 500000us (120 BPM):
  track 0 "Lead": quarter notes C4 E4 G4 C5 (note-off events)
  track 1 "Bass": whole note C2 via velocity-0 note-on (tests the off-by-vel convention)
Re-run after editing if you want byte-stable fixture regeneration.
"""

from pathlib import Path


def vlq(n: int) -> bytes:
    out = bytearray([n & 0x7F])
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def track(events: bytes) -> bytes:
    return b"MTrk" + len(events).to_bytes(4, "big") + events


lead = b"".join([
    vlq(0) + bytes([0xFF, 0x51, 0x03]) + (500000).to_bytes(3, "big"),   # tempo 120bpm
    vlq(0) + bytes([0xFF, 0x03, 0x04]) + b"Lead",                        # name
    vlq(0) + bytes([0x90, 60, 90]), vlq(480) + bytes([0x80, 60, 64]),    # C4
    vlq(0) + bytes([0x90, 64, 90]), vlq(480) + bytes([0x80, 64, 64]),    # E4
    vlq(0) + bytes([0x90, 67, 90]), vlq(480) + bytes([0x80, 67, 64]),    # G4
    vlq(0) + bytes([0x90, 72, 90]), vlq(480) + bytes([0x80, 72, 64]),    # C5
    vlq(0) + bytes([0xFF, 0x2F, 0x00]),
])

bass = b"".join([
    vlq(0) + bytes([0xFF, 0x03, 0x04]) + b"Bass",                        # name
    vlq(0) + bytes([0x90, 36, 90]),                                      # C2 on
    vlq(1920) + bytes([0x90, 36, 0]),                                    # off via vel-0 note-on
    vlq(0) + bytes([0xFF, 0x2F, 0x00]),
])

header = b"MThd" + (6).to_bytes(4, "big") + (1).to_bytes(2, "big") + (2).to_bytes(2, "big") + (480).to_bytes(2, "big")
out = Path(__file__).resolve().parent / "two_notes.mid"
out.write_bytes(header + track(lead) + track(bass))
print(f"wrote {out} ({len(out.read_bytes())} bytes)")
