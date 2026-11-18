#!/usr/bin/env bash
# roundtrip_eval.sh — AI-pipeline integration proof + MIDI-note F1 metric (plan §9).
#
# Chain: engine renders a known synth melody → basicpitch cartridge transcribes it →
# opd midi2opj imports the MIDI back as an editable project → we score pitch-content
# F1 of original vs imported. This is the hum→MIDI→synth product loop, measured.
#
# Requires OP_TRANSCRIBE_PYTHON (ONNX runtime interpreter; see backends/basicpitch/impl.py).
# Exits 0 if F1 >= threshold (default 0.8), 3 if skipped (no runtime), else 1.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPD="${OPD_BIN:-$ROOT/build/opd}"
THRESHOLD="${F1_THRESHOLD:-0.8}"

if [ -z "${OP_TRANSCRIBE_PYTHON:-}" ]; then
    echo "SKIP roundtrip_eval: OP_TRANSCRIBE_PYTHON not set"; exit 3
fi
WRAP="$ROOT/ai-sidecar/backends/basicpitch/run_transcribe.py"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

"$OPD" render "$ROOT/tests/fixtures/goldens/pitched.opj" -o "$TMP/melody.wav" >/dev/null || { echo "FAIL render"; exit 1; }
"$OP_TRANSCRIBE_PYTHON" "$WRAP" "$TMP/melody.wav" "$TMP/melody.mid" >/dev/null || { echo "FAIL transcribe"; exit 1; }
"$OPD" midi2opj "$TMP/melody.mid" -o "$TMP/roundtrip.opj" >/dev/null || { echo "FAIL import"; exit 1; }
"$OPD" render "$TMP/roundtrip.opj" -o "$TMP/roundtrip.wav" >/dev/null || { echo "FAIL re-render"; exit 1; }

python3 - "$ROOT/tests/fixtures/goldens/pitched.opj" "$TMP/melody.mid" "$THRESHOLD" <<'PYEOF'
import json, struct, sys

orig = json.load(open(sys.argv[1]))
want = sorted(n.get("note") for pat in orig["patterns"] for n in pat["notes"] if n.get("note", -1) >= 0)

# minimal SMF type-1 note reader (our own pipeline's output format)
data = open(sys.argv[2], "rb").read()
assert data[:4] == b"MThd"
hdrlen = struct.unpack(">I", data[4:8])[0]
fmt, ntrks, div = struct.unpack(">HHH", data[8:14])
ppq = float(div); pos = 8 + hdrlen; got = []
def vlq(p):
    v = 0
    while True:
        c = data[p]; p += 1; v = (v << 7) | (c & 0x7F)
        if not c & 0x80: return v, p
for _ in range(ntrks):
    assert data[pos:pos+4] == b"MTrk"
    ln = struct.unpack(">I", data[pos+4:pos+8])[0]; end = pos + 8 + ln; pos += 8
    tick = 0; running = 0; open_notes = {}
    while pos < end:
        d, pos = vlq(pos); tick += d
        st = data[pos]
        if st < 0x80: st = running
        else:
            pos += 1
            running = st if st < 0xF0 else running
        if st == 0xFF:
            mt = data[pos]; pos += 1; ml, pos = vlq(pos)
            if mt == 0x2F: break
            pos += ml
        elif 0x90 <= st < 0xA0:
            key, vel = data[pos], data[pos+1]; pos += 2
            if vel > 0: open_notes[key] = tick
            elif key in open_notes: got.append(key); del open_notes[key]
        elif 0x80 <= st < 0x90:
            key = data[pos]; pos += 2
            if key in open_notes: got.append(key); del open_notes[key]
        elif 0xC0 <= st < 0xE0 or 0xD0 <= st < 0xE0: pos += 1
        else: pos += 2

got = sorted(got)
if not want or not got:
    print(f"F1=0.0 (want {len(want)} notes, got {len(got)})"); sys.exit(1)
pool = {}
for k in got: pool[k] = pool.get(k, 0) + 1
hits = 0
for k in want:
    if pool.get(k, 0) > 0: hits += 1; pool[k] -= 1
prec = hits / len(got); rec = hits / len(want)
f1 = 2 * prec * rec / (prec + rec) if prec + rec else 0.0
thr = float(sys.argv[3])
print(f"roundtrip: want={want}")
print(f"roundtrip:  got={got}")
print(f"ROUNDTRIP F1={f1:.2f} (precision {prec:.2f}, recall {rec:.2f}) threshold {thr} -> {'PASS' if f1 >= thr else 'FAIL'}")
sys.exit(0 if f1 >= thr else 1)
PYEOF
rc=$?
exit $rc
