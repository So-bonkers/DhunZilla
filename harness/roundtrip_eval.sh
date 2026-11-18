#!/usr/bin/env bash
# roundtrip_eval.sh — AI-pipeline integration proof + note metrics (plan §9), corpus edition.
#
# Chain per case: engine renders a known synth melody → basicpitch cartridge transcribes it →
# scoring vs the project's own notes (harness/score_notes.py):
#   pitch F1  : multiset of pitches, timing-insensitive        (gate: F1_THRESHOLD, 0.8)
#   event F1  : pitch AND onset within TOL_S (default 0.08s)   (gate: EVENT_F1_THRESHOLD, 0.8)
# Default corpus: goldens/pitched.opj + tests/fixtures/eval/*.opj (register/transient/legato
# stress, WP-0127). Pass explicit .opj paths as args to override. Exits 3 (skip) without
# OP_TRANSCRIBE_PYTHON so CI-safe; exits 1 if any case fails its gates.
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

if [ "$#" -gt 0 ]; then CORPUS=("$@"); else
    CORPUS=("$ROOT/tests/fixtures/goldens/pitched.opj" "$ROOT"/tests/fixtures/eval/*.opj)
fi

fails=0
for opj in "${CORPUS[@]}"; do
    name="$(basename "$opj" .opj)"
    "$OPD" render "$opj" -o "$TMP/$name.wav" >/dev/null || { echo "$name: FAIL render"; fails=$((fails+1)); continue; }
    "$OP_TRANSCRIBE_PYTHON" "$WRAP" "$TMP/$name.wav" "$TMP/$name.mid" >/dev/null \
        || { echo "$name: FAIL transcribe"; fails=$((fails+1)); continue; }
    python3 "$ROOT/harness/score_notes.py" "$opj" "$TMP/$name.mid" \
        "$THRESHOLD" "$EVENT_THRESHOLD" "$TOL_S" "$name" || fails=$((fails+1))
    # product-loop smoke: MIDI must import back as an editable, renderable project
    "$OPD" midi2opj "$TMP/$name.mid" -o "$TMP/$name.rt.opj" >/dev/null \
        && "$OPD" render "$TMP/$name.rt.opj" -o "$TMP/$name.rt.wav" >/dev/null \
        || { echo "$name: FAIL import/render loop"; fails=$((fails+1)); }
done

echo "roundtrip_eval: ${#CORPUS[@]} cases, $fails failures"
[ "$fails" -eq 0 ] || exit 1
