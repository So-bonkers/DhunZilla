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
PROMPT="${1:-}"
[ -n "$PROMPT" ] || { echo "usage: $0 \"text prompt\" [outdir]"; exit 2; }
OUT="${2:-$PWD/prompt_song}"
TMP="$(mktemp -d)"
cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; wait "${SRV:-}" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

GEN="${OP_GEN_BACKEND:-mockgen}"
if [ -n "${OP_TRANSCRIBE_PYTHON:-}" ]; then DEF_TR=basicpitch; else DEF_TR=mocktrans; fi
TR="${OP_TRANSCRIBE_BACKEND:-$DEF_TR}"
if [ "$TR" = "basicpitch" ] && [ -z "${OP_TRANSCRIBE_PYTHON:-}" ]; then
    echo "SKIP: basicpitch requested but OP_TRANSCRIBE_PYTHON not set"; exit 3
fi

cd "$ROOT/ai-sidecar"
uv run opsidecar serve --port 0 --jobs-dir "$TMP/jobs" > "$TMP/serve.log" 2>&1 &
SRV=$!
PORT=""
for _ in $(seq 1 60); do
    PORT=$(grep -o 'SIDECAR_PORT=[0-9]*' "$TMP/serve.log" | head -1 | cut -d= -f2 || true)
    [ -n "$PORT" ] && break
    sleep 0.5
done
[ -n "$PORT" ] || { echo "FAIL: sidecar never announced a port"; tail -20 "$TMP/serve.log"; exit 1; }

python3 - "http://127.0.0.1:$PORT" "$TMP" "$GEN" "$TR" "$PROMPT" <<'PYEOF'
import json, pathlib, sys, time, urllib.request

base, tmp, gen_backend, tr_backend, prompt = sys.argv[1:6]


def req(method, path, body=None):
    rq = urllib.request.Request(
        base + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(rq) as r:
        return json.loads(r.read().decode())


def wait_done(jid):
    for _ in range(300):
        s = req("GET", f"/jobs/{jid}")
        if s["state"] in ("done", "failed", "cancelled"):
            return s
        time.sleep(2)
    raise SystemExit(f"job {jid} timed out")


gen = wait_done(req("POST", "/jobs", {"backend": gen_backend, "task": "generate",
                                      "params": {"prompt": prompt, "duration_s": 4.0,
                                                 "seed": 2026}})["job_id"])
assert gen["state"] == "done", f"generate failed: {gen.get('error')}"
src_wav = next(iter(gen["result"]["artifacts"].values()))
gen_prov = json.loads(open(gen["result"]["provenance_file"]).read())

tr = wait_done(req("POST", "/jobs", {"backend": tr_backend, "task": "transcribe",
                                     "params": {"audio_path": src_wav}})["job_id"])
assert tr["state"] == "done", f"transcribe failed: {tr.get('error')}"
src_mid = tr["result"]["artifacts"]["midi"]
tr_prov = json.loads(open(tr["result"]["provenance_file"]).read())

out = pathlib.Path(tmp, "chain.json")
out.write_text(json.dumps({
    "wav": src_wav, "mid": str(src_mid),
    "gen_producer": f"{gen_backend}@{gen_prov.get('model_version', '?')}",
    "tr_producer": f"{tr_backend}@{tr_prov.get('model_version', '?')}",
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
