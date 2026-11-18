#!/usr/bin/env bash
# midi_into_project.sh — transcribe audio via the sidecar and merge the result into a project
# as editable tracks, attaching the MIDI with provenance (WP-0119). The hum→MIDI flagship:
# sing/hum/record → Basic Pitch (or mocktrans in CI) → notes land beside your drums.
#
# usage: harness/midi_into_project.sh <proj.opj> <audio.wav>
# backend: $OP_TRANSCRIBE_BACKEND (default: basicpitch when OP_TRANSCRIBE_PYTHON is set,
#          else mocktrans — CI-safe). basicpitch without its runtime → exit 3 (gate convention).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="${1:-}"; AUDIO="${2:-}"
[ -f "$PROJ" ] || { echo "usage: $0 <proj.opj> <audio.wav>"; exit 2; }
[ -f "$AUDIO" ] || { echo "FAIL: audio not found: $AUDIO"; exit 2; }
TMP="$(mktemp -d)"
cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; wait "${SRV:-}" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

if [ -n "${OP_TRANSCRIBE_PYTHON:-}" ]; then DEFAULT_BACKEND=basicpitch; else DEFAULT_BACKEND=mocktrans; fi
BACKEND="${OP_TRANSCRIBE_BACKEND:-$DEFAULT_BACKEND}"
if [ "$BACKEND" = "basicpitch" ] && [ -z "${OP_TRANSCRIBE_PYTHON:-}" ]; then
    echo "SKIP: basicpitch backend requested but OP_TRANSCRIBE_PYTHON not set"; exit 3
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

python3 - "http://127.0.0.1:$PORT" "$PROJ" "$AUDIO" "$BACKEND" "$ROOT/build/opd" <<'PYEOF'
import json, pathlib, shutil, subprocess, sys, time, urllib.request

base, proj, audio, backend, opd = sys.argv[1:6]


def req(method, path, body=None):
    rq = urllib.request.Request(
        base + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(rq) as r:
        return json.loads(r.read().decode())


jid = req("POST", "/jobs", {"backend": backend, "task": "transcribe",
                            "params": {"audio_path": audio}})["job_id"]
snap = None
for _ in range(300):  # real ONNX on short inputs: seconds; ceiling covers long ones
    snap = req("GET", f"/jobs/{jid}")
    if snap["state"] in ("done", "failed", "cancelled"):
        break
    time.sleep(2)
assert snap and snap["state"] == "done", f"job not done: {snap}"

prov = json.loads(open(snap["result"]["provenance_file"]).read())
producer = f"{backend}@{prov.get('model_version', '?')}"
src_midi = pathlib.Path(snap["result"]["artifacts"]["midi"])

rel = "ai/transcribed.mid"
dst = pathlib.Path(proj).parent / rel
dst.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(src_midi, dst)

r = subprocess.run([opd, "import-midi", proj, str(dst)], capture_output=True, text=True)
assert r.returncode == 0, f"import-midi failed: {r.stderr.strip()}"
r = subprocess.run([opd, "attach", proj, "--path", rel, "--kind", "transcription",
                    "--producer", producer], capture_output=True, text=True)
assert r.returncode == 0, f"attach failed: {r.stderr.strip()}"
print(f"transcribed via {producer}: notes merged into {proj}, MIDI attached at {rel}")
PYEOF
rc=$?
[ $rc -eq 0 ] && "$ROOT/build/opd" info "$PROJ" | tail -4
exit $rc
