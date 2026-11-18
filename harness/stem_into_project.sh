#!/usr/bin/env bash
# stem_into_project.sh — separate audio into stems via the sidecar and attach them to a project
# with provenance (WP-0118). The end-to-end "AI did something only AI can do, and the project
# remembers" workflow: every produced stem lands in <proj-dir>/stems/<name>.wav and gets an
# `opd attach` entry ("<backend>@<model_version>").
#
# usage: harness/stem_into_project.sh <proj.opj> [audio.wav]
#        (audio defaults to rendering the project itself)
# backend: $OP_STEM_BACKEND (default: demucs when OP_DEMUCS_PYTHON is set, else mocksep —
#          CI-safe with zero ML deps). demucs without its runtime → exit 3 (gate convention).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJ="${1:-}"
[ -f "$PROJ" ] || { echo "usage: $0 <proj.opj> [audio.wav]"; exit 2; }
TMP="$(mktemp -d)"
cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; wait "${SRV:-}" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

if [ -n "${OP_DEMUCS_PYTHON:-}" ]; then DEFAULT_BACKEND=demucs; else DEFAULT_BACKEND=mocksep; fi
BACKEND="${OP_STEM_BACKEND:-$DEFAULT_BACKEND}"
if [ "$BACKEND" = "demucs" ] && [ -z "${OP_DEMUCS_PYTHON:-}" ]; then
    echo "SKIP: demucs backend requested but OP_DEMUCS_PYTHON not set"; exit 3
fi

AUDIO="${2:-}"
if [ -z "$AUDIO" ]; then
    AUDIO="$TMP/self_render.wav"
    "$ROOT/build/opd" render "$PROJ" -o "$AUDIO" >/dev/null || { echo "FAIL: self-render"; exit 1; }
fi
[ -f "$AUDIO" ] || { echo "FAIL: audio not found: $AUDIO"; exit 2; }

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
import json, shutil, subprocess, sys, time, urllib.request

base, proj, audio, backend, opd = sys.argv[1:6]


def req(method, path, body=None):
    rq = urllib.request.Request(
        base + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(rq) as r:
        return json.loads(r.read().decode())


jid = req("POST", "/jobs", {"backend": backend, "task": "separate",
                            "params": {"audio_path": audio}})["job_id"]
# Generous ceiling: demucs on CPU is ~1.6x realtime + model load; mocks finish instantly.
snap = None
for _ in range(300):
    snap = req("GET", f"/jobs/{jid}")
    if snap["state"] in ("done", "failed", "cancelled"):
        break
    time.sleep(2)
assert snap and snap["state"] == "done", f"job not done: {snap}"

prov = json.loads(open(snap["result"]["provenance_file"]).read())
producer = f"{backend}@{prov.get('model_version', '?')}"
artifacts = dict(sorted(snap["result"]["artifacts"].items()))
assert artifacts, "no artifacts in result"

n = 0
for name, src in artifacts.items():
    rel = f"stems/{name}.wav"
    import pathlib
    dst = pathlib.Path(proj).parent / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    r = subprocess.run([opd, "attach", proj, "--path", rel, "--kind", "stem",
                        "--producer", producer], capture_output=True, text=True)
    assert r.returncode == 0, f"attach failed for {name}: {r.stderr.strip()}"
    n += 1
print(f"attached {n} stem(s) from {producer} to {proj}")
PYEOF
rc=$?
[ $rc -eq 0 ] && "$ROOT/build/opd" info "$PROJ" | tail -5
exit $rc
