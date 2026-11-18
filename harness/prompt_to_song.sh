#!/usr/bin/env bash
# prompt_to_song.sh — the flagship chain end to end (WP-0124): a text prompt becomes an EDITABLE
# project, not just a blob of audio.
#
#   prompt → [generate backend] → audio → [transcribe backend] → MIDI → opd midi2opj → .opj
#          → attach source audio + MIDI with provenance → render final WAV
#
# CI-safe by default (mockgen + mocktrans). Real backends: set OP_GEN_BACKEND /
# OP_TRANSCRIBE_PYTHON per the other glue scripts. usage: prompt_to_song.sh "your prompt" [outdir]
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in -h|--help) sed -n '2,9p' "$0"; exit 0;; esac
PROMPT="${1:-}"
[ -n "$PROMPT" ] || { echo "usage: $0 \"text prompt\" [outdir]   (-h for details)"; exit 2; }
OUT="${2:-$PWD/prompt_song}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

GEN="${OP_GEN_BACKEND:-mockgen}"
if [ -n "${OP_TRANSCRIBE_PYTHON:-}" ]; then DEF_TR=basicpitch; else DEF_TR=mocktrans; fi
TR="${OP_TRANSCRIBE_BACKEND:-$DEF_TR}"
if [ "$TR" = "basicpitch" ] && [ -z "${OP_TRANSCRIBE_PYTHON:-}" ]; then
    echo "SKIP: basicpitch requested but OP_TRANSCRIBE_PYTHON not set"; exit 3
fi

OP_ROOT="$ROOT" python3 - "$TMP" "$GEN" "$TR" "$PROMPT" <<'PYEOF'
import json, os, pathlib, sys

sys.path.insert(0, os.environ["OP_ROOT"] + "/harness")
from job_client import provenance_producer, start_sidecar, submit, wait_done

tmp, gen_backend, tr_backend, prompt = sys.argv[1:5]
_op, base = start_sidecar(os.environ["OP_ROOT"], tmp + "/jobs")

gen = wait_done(base, submit(base, gen_backend, "generate",
                             {"prompt": prompt, "duration_s": 4.0, "seed": 2026}))
src_wav = next(iter(gen["result"]["artifacts"].values()))
tr = wait_done(base, submit(base, tr_backend, "transcribe", {"audio_path": src_wav}))

out = pathlib.Path(tmp, "chain.json")
out.write_text(json.dumps({
    "wav": src_wav, "mid": str(tr["result"]["artifacts"]["midi"]),
    "gen_producer": provenance_producer(gen, gen_backend),
    "tr_producer": provenance_producer(tr, tr_backend),
}))
print("chain jobs done")
PYEOF
rc=$?
[ $rc -ne 0 ] && exit $rc

# Land the artifacts in the song dir and build the editable project with full provenance.
mkdir -p "$OUT/ai"
python3 - "$TMP/chain.json" "$OUT" <<'PYEOF'
import json, pathlib, shutil, sys

chain = json.loads(pathlib.Path(sys.argv[1]).read_text())
out = pathlib.Path(sys.argv[2])
shutil.copyfile(chain["wav"], out / "ai/source_audio.wav")
shutil.copyfile(chain["mid"], out / "ai/transcribed.mid")
pathlib.Path(out / "ai/producers.txt").write_text(
    chain["gen_producer"] + "\n" + chain["tr_producer"] + "\n")
PYEOF

"$ROOT/build/opd" midi2opj "$OUT/ai/transcribed.mid" -o "$OUT/song.opj" >/dev/null || exit 1
GEN_PROD=$(sed -n 1p "$OUT/ai/producers.txt"); TR_PROD=$(sed -n 2p "$OUT/ai/producers.txt")
"$ROOT/build/opd" attach "$OUT/song.opj" --path ai/source_audio.wav --kind other \
    --producer "$GEN_PROD" >/dev/null || exit 1
"$ROOT/build/opd" attach "$OUT/song.opj" --path ai/transcribed.mid --kind transcription \
    --producer "$TR_PROD" >/dev/null || exit 1
"$ROOT/build/opd" render "$OUT/song.opj" -o "$OUT/final_render.wav" >/dev/null || exit 1

echo "prompt -> editable song: $OUT"
"$ROOT/build/opd" info "$OUT/song.opj" | tail -3
ls "$OUT"
