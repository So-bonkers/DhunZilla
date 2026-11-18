#!/usr/bin/env bash
# roundtrip_eval.sh — AI-pipeline integration proof + note metrics (plan §9).
#
# Chain: engine renders a known synth melody → basicpitch cartridge transcribes it →
# opd midi2opj imports the MIDI back as an editable project → we score.
#   pitch F1  : multiset of pitches, timing-insensitive        (gate: F1_THRESHOLD, 0.8)
#   event F1  : pitch AND onset within TOL_S (default 0.08s)   (gate: EVENT_F1_THRESHOLD, 0.8; measured deterministic ±25ms drift)
# This is the hum→MIDI→synth product loop, measured. Exits 3 (skip) without
# OP_TRANSCRIBE_PYTHON so CI-safe.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPD="${OPD_BIN:-$ROOT/build/opd}"
THRESHOLD="${F1_THRESHOLD:-0.8}"
EVENT_THRESHOLD="${EVENT_F1_THRESHOLD:-0.8}"
TOL_S="${TOL_S:-0.08}"

if [ -z "${OP_TRANSCRIBE_PYTHON:-}" ]; then
    echo "SKIP roundtrip_eval: OP_TRANSCRIBE_PYTHON not set"; exit 3
fi
WRAP="$ROOT/ai-sidecar/backends/basicpitch/run_transcribe.py"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

"$OPD" render "$ROOT/tests/fixtures/goldens/pitched.opj" -o "$TMP/melody.wav" >/dev/null || { echo "FAIL render"; exit 1; }
"$OP_TRANSCRIBE_PYTHON" "$WRAP" "$TMP/melody.wav" "$TMP/melody.mid" >/dev/null || { echo "FAIL transcribe"; exit 1; }
"$OPD" midi2opj "$TMP/melody.mid" -o "$TMP/roundtrip.opj" >/dev/null || { echo "FAIL import"; exit 1; }
"$OPD" render "$TMP/roundtrip.opj" -o "$TMP/roundtrip.wav" >/dev/null || { echo "FAIL re-render"; exit 1; }

python3 - "$ROOT/tests/fixtures/goldens/pitched.opj" "$TMP/melody.mid" "$THRESHOLD" "$EVENT_THRESHOLD" "$TOL_S" <<'PYEOF'
import json, struct, sys

orig_path, mid_path, pitch_thr, event_thr, tol = (sys.argv[1], sys.argv[2], float(sys.argv[3]),
                                                  float(sys.argv[4]), float(sys.argv[5]))

# --- expected events straight from the source project (constant tempo) ---------------
orig = json.load(open(orig_path))
spb = 60.0 / float(orig["bpm"])
want_ev = sorted((n["t"] * spb, n["note"]) for pat in orig["patterns"] for n in pat["notes"]
                 if n.get("note", -1) >= 0)
want = sorted(k for _, k in want_ev)

# --- detected events from the transcribed MIDI (minimal SMF reader) ------------------
data = open(mid_path, "rb").read()
assert data[:4] == b"MThd"
hdrlen = struct.unpack(">I", data[4:8])[0]
_fmt, ntrks, div = struct.unpack(">HHH", data[8:14])
ppq = float(div)
pos = 8 + hdrlen


def vlq(p):
    v = 0
    while True:
        c = data[p]; p += 1; v = (v << 7) | (c & 0x7F)
        if not c & 0x80:
            return v, p


tempo_us = 500000.0  # SMF default
got_ev = []
for _ in range(ntrks):
    assert data[pos:pos + 4] == b"MTrk"
    ln = struct.unpack(">I", data[pos + 4:pos + 8])[0]
    end = pos + 8 + ln
    pos += 8
    tick = 0
    running = 0
    open_notes = {}
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
            mt = data[pos]; pos += 1
            ml, pos = vlq(pos)
            if mt == 0x51 and ml >= 3 and tempo_us == 500000.0:
                tempo_us = float(int.from_bytes(data[pos:pos + 3], "big"))
            elif mt == 0x2F:
                break
            pos += ml
        elif 0x90 <= st < 0xA0:
            key, vel = data[pos], data[pos + 1]; pos += 2
            if vel > 0:
                open_notes.setdefault(key, tick)
            elif key in open_notes:
                got_ev.append((open_notes.pop(key) / ppq * tempo_us / 1e6, key))
        elif 0x80 <= st < 0x90:
            key = data[pos]; pos += 2
            if key in open_notes:
                got_ev.append((open_notes.pop(key) / ppq * tempo_us / 1e6, key))
        elif 0xC0 <= st < 0xE0:
            pos += 1
        else:
            pos += 2

got = sorted(k for _, k in got_ev)

# --- metric 1: pitch multiset F1 ------------------------------------------------------
def multiset_f1(want, got):
    if not want or not got:
        return 0.0
    pool = {}
    for k in got:
        pool[k] = pool.get(k, 0) + 1
    hits = 0
    for k in want:
        if pool.get(k, 0) > 0:
            hits += 1
            pool[k] -= 1
    p, r = hits / len(got), hits / len(want)
    return 2 * p * r / (p + r) if p + r else 0.0


# --- metric 2: event F1 (pitch exact, onset within tol; greedy nearest) --------------
def event_f1(want_ev, got_ev, tol):
    remaining = list(got_ev)
    hits = 0
    for w in sorted(want_ev):
        best_i, best_d = -1, tol + 1e-9
        for i, g in enumerate(remaining):
            if g[1] == w[1]:
                d = abs(g[0] - w[0])
                if d <= tol and d < best_d:
                    best_i, best_d = i, d
        if best_i >= 0:
            hits += 1
            remaining.pop(best_i)
    p = hits / len(got_ev) if got_ev else 0.0
    r = hits / len(want_ev) if want_ev else 0.0
    return 2 * p * r / (p + r) if p + r else 0.0


f1 = multiset_f1(want, got)
ef1 = event_f1(want_ev, got_ev, tol)
print(f"want={[(round(t,3),k) for t,k in want_ev]}")
print(f"got ={[(round(t,3),k) for t,k in sorted(got_ev)]}")
ok1 = f1 >= pitch_thr
ok2 = ef1 >= event_thr
print(f"PITCH F1={f1:.2f} (thr {pitch_thr}) {'PASS' if ok1 else 'FAIL'}")
print(f"EVENT F1={ef1:.2f} (tol {tol}s, thr {event_thr}) {'PASS' if ok2 else 'FAIL'}")
sys.exit(0 if (ok1 and ok2) else 1)
PYEOF
exit $?
