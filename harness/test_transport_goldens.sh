#!/usr/bin/env bash
# test_transport_goldens.sh — WP-0242 transport coverage: the 3/4 project with a mid-song tempo drop
# (tests/fixtures/transport/transport_3_4_tempo.opj), its bit-exact `opd click` golden, the bar/beat
# ruler values, and the accent rule (every bar's FIRST beat accented in 3/4, nowhere else).
# Output style follows harness/test_cli_errors.sh: one PASS/FAIL line per assertion and a final
# "test_transport_goldens: N failures"; exit 0 iff N = 0. Nothing else exits the script early.
# Usage: harness/test_transport_goldens.sh [path/to/opd]   (default build/opd)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPD="${1:-$ROOT/build/opd}"
FIX="$ROOT/tests/fixtures/transport"
OPJ="$FIX/transport_3_4_tempo.opj"
OPJL="$FIX/transport_3_4_tempo.opjl"
SHA="$FIX/transport_3_4_tempo.click.sha256"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0
ok()  { echo "PASS $1"; }
bad() { echo "FAIL $1: $2"; fails=$((fails+1)); }

# 1. The three fixture files exist and are non-empty.
for f in "$OPJL" "$OPJ" "$SHA"; do
    if [ -s "$f" ]; then ok "fixture_present_$(basename "$f")"; else bad "fixture_present_$(basename "$f")" "missing or empty: $f"; fi
done

# 2. Provenance: the committed generated fixtures ARE the make script's recipe, byte-for-byte.
bash "$ROOT/harness/make_transport_golden.sh" "$OPD" "$TMP/regen" > "$TMP/regen.log" 2>&1; mk=$?
if [ "$mk" = 0 ]; then ok "make_recipe_ran (exit 0)"; else bad "make_recipe_ran" "exit $mk — $(tail -n 3 "$TMP/regen.log" | tr '\n' ' ')"; fi
if [ -s "$TMP/regen/transport_3_4_tempo.opj" ] && cmp -s "$TMP/regen/transport_3_4_tempo.opj" "$OPJ"; then
    ok "regen_opj_byte_identical"
else
    bad "regen_opj_byte_identical" "regenerated .opj differs from $OPJ"
fi
if [ -s "$TMP/regen/transport_3_4_tempo.click.sha256" ] && cmp -s "$TMP/regen/transport_3_4_tempo.click.sha256" "$SHA"; then
    ok "regen_click_digest_byte_identical"
else
    bad "regen_click_digest_byte_identical" "regenerated digest differs from $SHA"
fi

# 3. opd click: two renders byte-identical (run-to-run determinism), digest == committed golden.
"$OPD" click "$OPJ" -o "$TMP/click1.wav" >/dev/null 2>&1; r1=$?
"$OPD" click "$OPJ" -o "$TMP/click2.wav" >/dev/null 2>&1; r2=$?
if [ "$r1" = 0 ] && [ "$r2" = 0 ] && cmp -s "$TMP/click1.wav" "$TMP/click2.wav"; then
    ok "click_deterministic (two renders identical)"
else
    bad "click_deterministic" "r1=$r1 r2=$r2 renders differ or failed"
fi
got_sha="$(sha256sum "$TMP/click1.wav" 2>/dev/null | cut -d' ' -f1)"
want_sha="$(tr -d '[:space:]' < "$SHA")"
if [ -n "$got_sha" ] && [ "$got_sha" = "$want_sha" ]; then
    ok "click_bit_exact_golden (sha256 $got_sha)"
else
    bad "click_bit_exact_golden" "render sha256 [$got_sha] != committed [$want_sha]"
fi

# 4. opd timeline: 3/4, 4 bars, tempo drop at bar 2, bar start samples pinned by the task.
"$OPD" timeline "$OPJ" > "$TMP/timeline.json" 2>/dev/null
tl="$(python3 - "$TMP/timeline.json" <<'PYEOF'
import json, sys
try:
    j = json.load(open(sys.argv[1]))
except Exception as e:
    print("bad timeline JSON: %r" % (e,)); sys.exit(0)
errs = []
if j.get("sample_rate") != 48000:                      errs.append("sample_rate %r" % (j.get("sample_rate"),))
if j.get("beats_per_bar") != 3:                       errs.append("beats_per_bar %r" % (j.get("beats_per_bar"),))
if j.get("time_signature") != "3/4":                  errs.append("time_signature %r" % (j.get("time_signature"),))
if j.get("length_bars") != 4.0:                       errs.append("length_bars %r" % (j.get("length_bars"),))
bars = j.get("bars", [])
if len(bars) != 4:                                    errs.append("bars count %r" % (len(bars),))
if [b["start_beat"] for b in bars] != [0.0, 3.0, 6.0, 9.0]:           errs.append("start_beat %r" % ([b.get("start_beat") for b in bars],))
if [b["start_sample"] for b in bars] != [0, 72000, 144000, 240000]:   errs.append("start_sample %r" % ([b.get("start_sample") for b in bars],))
if [b["bpm"] for b in bars] != [120.00, 120.00, 90.00, 90.00]:        errs.append("bpm %r (tempo drop must be at bar 2)" % ([b.get("bpm") for b in bars],))
print("OK" if not errs else "detail: " + "; ".join(errs))
PYEOF
)"
if [ "$tl" = "OK" ]; then
    ok "timeline_ruler (3/4, 4 bars, 3 beats/bar, bpm 120,120,90,90, start samples 0,72000,144000,240000)"
else
    bad "timeline_ruler" "${tl:-no output from python check}"
fi

# 5. The click analysis (task §"The click analysis"): geometry, onsets, accent counts, accent
#    positions == bar first beats, silence after the last burst. stdlib wave+array only.
wa="$(python3 - "$TMP/click1.wav" <<'PYEOF'
import array, sys, wave
try:
    w = wave.open(sys.argv[1], "rb")
    frames = array.array("h")
    frames.frombytes(w.readframes(w.getnframes()))
    left, right = frames[0::2], frames[1::2]
    sr = 1440  # BURST = sample_rate * 30 / 1000 = 48000 * 30 / 1000 (pinned below against the header)
    print("GEO %d %d %d %d %d" % (w.getnchannels(), w.getsampwidth() * 8, w.getframerate(), w.getnframes(), 1 if left == right else 0))
    onsets = [i for i, v in enumerate(left) if v == 16384]
    print("ONSETS " + ",".join(str(i) for i in onsets))
    counts = []
    for o in onsets:
        c = 0
        prev = 0
        for v in left[o:o + sr]:
            if v == 0:
                continue
            if prev != 0 and (v < 0) != (prev < 0):
                c += 1
            prev = v
        counts.append(c)
    print("COUNTS " + ",".join(str(c) for c in counts))
    accents = [o for o, c in zip(onsets, counts) if c >= 75]
    print("ACCENTS " + ",".join(str(a) for a in accents))
    end = 304000 + 1440
    print("TAIL_SILENT %d" % (1 if end <= len(left) and all(v == 0 for v in left[end:]) else 0))
except Exception as e:
    print("ERROR %r" % (e,))
PYEOF
)"
geo="$(printf '%s\n' "$wa" | sed -n 's/^GEO //p')"
[ "$geo" = "2 16 48000 336000 1" ] \
    && ok "wav_geometry (2ch, 16-bit, 48000 Hz, 336000 frames, L==R)" \
    || bad "wav_geometry" "want [2 16 48000 336000 1] got [$geo] ($(printf '%s\n' "$wa" | grep ^ERROR))"
onsets="$(printf '%s\n' "$wa" | sed -n 's/^ONSETS //p')"
[ "$onsets" = "0,24000,48000,72000,96000,120000,144000,176000,208000,240000,272000,304000" ] \
    && ok "click_onsets_12_exact (tempo drop visible between beats 5 and 6)" \
    || bad "click_onsets_12_exact" "got [$onsets]"
counts="$(printf '%s\n' "$wa" | sed -n 's/^COUNTS //p')"
[ "$counts" = "90,60,60,90,60,60,90,60,60,90,60,60" ] \
    && ok "accent_counts_exact (1500 Hz downbeats=90 sign changes, 1000 Hz beats=60)" \
    || bad "accent_counts_exact" "got [$counts]"
accents="$(printf '%s\n' "$wa" | sed -n 's/^ACCENTS //p')"
[ "$accents" = "0,72000,144000,240000" ] \
    && ok "accents_on_bar_first_beats (exactly the 4 bar starts in 3/4, count >= 75)" \
    || bad "accents_on_bar_first_beats" "got [$accents]"
tail_silent="$(printf '%s\n' "$wa" | sed -n 's/^TAIL_SILENT //p')"
[ "$tail_silent" = "1" ] \
    && ok "silence_after_last_burst (left[305440:] all zero)" \
    || bad "silence_after_last_burst" "got [$tail_silent]"

echo "test_transport_goldens: $fails failures"
if [ "$fails" -gt 0 ]; then exit 1; else exit 0; fi
