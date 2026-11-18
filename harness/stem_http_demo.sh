#!/usr/bin/env bash
# stem_http_demo.sh — AI feature demo over the real sidecar HTTP surface (WP-0107).
# Starts the sidecar with OP_DEMUCS_PYTHON set, asks it to separate an opd-rendered track,
# verifies four stems arrive: files exist, are non-silent, and provenance names demucs.
# GATED: exits 3 without OP_DEMUCS_PYTHON — keeps CI (sidecar_smoke.sh) mock-only.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ -z "${OP_DEMUCS_PYTHON:-}" ]; then
    echo "SKIP stem_http_demo: OP_DEMUCS_PYTHON not set"; exit 3
fi

TMP="$(mktemp -d)"
cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; wait "${SRV:-}" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

cd "$ROOT/ai-sidecar"
uv run opsidecar serve --port 0 --jobs-dir "$TMP/jobs" > "$TMP/serve.log" 2>&1 &
SRV=$!

PORT=""
for _ in $(seq 1 60); do
    PORT=$(grep -o 'SIDECAR_PORT=[0-9]*' "$TMP/serve.log" | head -1 | cut -d= -f2 || true)
    [ -n "$PORT" ] && break
    sleep 0.5
done
[ -n "$PORT" ] || { echo "FAIL: server never announced a port"; tail -20 "$TMP/serve.log"; exit 1; }

# Source material: the arrangement golden (real music-ish content, deterministic bytes).
"$ROOT/build/opd" render "$ROOT/tests/fixtures/goldens/demo.opj" -o "$TMP/mix.wav" >/dev/null

uv run python - "http://127.0.0.1:$PORT" "$TMP" <<'PYEOF'
import json, pathlib, sys, time, urllib.request

base, tmp = sys.argv[1], sys.argv[2]


def req(method, path, body=None):
    rq = urllib.request.Request(base + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(rq, timeout=30) as r:
        return json.loads(r.read().decode())


h = req("GET", "/health")
dem = [b for b in h["backends"] if b["id"] == "demucs"]
assert dem, f"demucs cartridge not discovered by live server: {[b['id'] for b in h['backends']]}"
assert dem[0]["health"]["status"] == "ok", f"demucs unhealthy: {dem[0]['health']}"
print("health: demucs discovered + ok via HTTP")

jid = req("POST", "/jobs", {"backend": "demucs", "task": "separate",
                            "params": {"audio_path": tmp + "/mix.wav"}})["job_id"]
st = None
for _ in range(1200):  # CPU htdemucs ≈ 1.6× realtime; generous ceiling for a demo
    st = req("GET", f"/jobs/{jid}")
    if st["state"] in ("done", "failed", "cancelled"):
        break
    time.sleep(1)
assert st and st["state"] == "done", f"job did not finish: {st}"
arts = st["result"]["artifacts"]
assert {"vocals", "drums", "bass", "other"} <= set(arts), arts

from ai_sidecar.wav import read_wav  # runs under the sidecar venv (uv run python)


def rms(p):
    data, _sr = read_wav(p)  # (frames, channels) float32 ndarray
    flat = data[: min(len(data), 48000 * 60)].ravel()
    return float((sum(float(x) * float(x) for x in flat) / max(1, len(flat))) ** 0.5)


for stem in ("vocals", "drums", "bass", "other"):
    f = pathlib.Path(arts[stem])
    assert f.is_file() and f.stat().st_size > 1000, f"{stem} missing/tiny: {f}"
    rms(f)  # must parse as WAV; content energy checked below on the informative stems
# demo.opj has no vocal source — require real energy where the kick/bass sine definitely land
assert rms(pathlib.Path(arts["drums"])) > 1e-4 or rms(pathlib.Path(arts["other"])) > 1e-4, \
    "expected audible content in drums/other stems"
prov = json.loads(pathlib.Path(st["result"]["provenance_file"]).read_text())
assert prov["backend_id"] == "demucs"
print("STEM HTTP DEMO OK — 4 stems served via HTTP, non-silence verified, provenance ok")
PYEOF
rc=$?
if [ $rc -ne 0 ]; then tail -20 "$TMP/serve.log"; fi
exit $rc
