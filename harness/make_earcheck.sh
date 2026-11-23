#!/usr/bin/env bash
# make_earcheck.sh — regenerate the human ear-check pack into demos/earcheck/ (gitignored).
# Everything is deterministic: same bytes as the goldens where they overlap.
# Optional: with OP_TRANSCRIBE_PYTHON set, also produces the AI round-trip audio (05/06).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/demos/earcheck"; mkdir -p "$OUT"
G="$ROOT/tests/fixtures/goldens"

"$ROOT/build/opd" render "$G/tempo.opj"     -o "$OUT/01_tempo_drop.wav"
"$ROOT/build/opd" render "$G/pitched.opj"   -o "$OUT/02_melody_synth.wav"
"$ROOT/build/opd" render "$G/imported.opj"  -o "$OUT/03_midi_imported.wav"
"$ROOT/build/opd" render "$G/panorama.opj"  -o "$OUT/04_panorama_mix.wav"
# WP-0197: the Phase 1 exit-criterion song "Exit Beat" (3-minute beat, built from `opd new` + an op
# journal in tests/fixtures/songs/). Writes 07_exit_beat_mix.wav (+ phase1_exit_stems/ and
# 08_exit_beat_bass_solo.wav) into $OUT; the mix is byte-identical to tests/fixtures/songs/phase1_beat.wav.
bash "$ROOT/harness/make_phase1_song.sh" --earcheck "$ROOT/build/opd" || echo "WARN: exit-song pack failed"

if [ -n "${OP_TRANSCRIBE_PYTHON:-}" ]; then
    echo "AI round-trip (needs ~15s ONNX inference)..."
    "$OP_TRANSCRIBE_PYTHON" "$ROOT/ai-sidecar/backends/basicpitch/run_transcribe.py" \
        "$OUT/02_melody_synth.wav" "$OUT/05_transcribed.mid" >/dev/null 2>&1 && \
    "$ROOT/build/opd" midi2opj "$OUT/05_transcribed.mid" -o "$OUT/05.opj" >/dev/null && \
    "$ROOT/build/opd" render "$OUT/05.opj" -o "$OUT/06_reconstructed_from_audio.wav" >/dev/null && \
    echo "05/06 written: 02 → transcribe → import → re-render (compare 02 vs 06)" || \
    echo "WARN: round-trip failed — check OP_TRANSCRIBE_PYTHON runtime"
else
    echo "(set OP_TRANSCRIBE_PYTHON to also generate the AI round-trip files 05/06)"
fi
echo "ear-check pack ready in demos/earcheck/"
