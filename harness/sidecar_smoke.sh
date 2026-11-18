#!/usr/bin/env bash
# sidecar_smoke.sh — live HTTP smoke test of the sidecar surface (WP-0106).
# Real uvicorn server on an ephemeral port: health → mockgen generate → mocksep separate
# (input rendered by our own opd binary) → artifacts + provenance verified on disk → error
# path. Mock backends only, so this is CI-safe with zero ML dependencies.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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

curl -fsS "http://127.0.0.1:$PORT/health" -o "$TMP/health.json" || { echo "FAIL: health request"; exit 1; }

# Input audio comes from our own engine — also proves opd↔sidecar file interop.
"$ROOT/build/opd" gen-testproject "$TMP/demo.opj" >/dev/null
"$ROOT/build/opd" render "$TMP/demo.opj" -o "$TMP/in.wav" >/dev/null

python3 - "http://127.0.0.1:$PORT" "$TMP" <<'PYEOF'
import json, pathlib, sys, time, urllib.error, urllib.request

base, tmp = sys.argv[1], sys.argv[2]


def req(method, path, body=None):
    rq = urllib.request.Request(
        base + path, method=method,
        data=json.dumps(body).encode() if body is not None else None,
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(rq, timeout=30) as r:
        return json.loads(r.read().decode())


def wait(job_id):
    for _ in range(240):
        st = req("GET", f"/jobs/{job_id}")
        if st["state"] in ("done", "failed", "cancelled"):
            return st
        time.sleep(0.5)
    raise SystemExit(f"FAIL: job {job_id} never finished")


h = json.load(open(tmp + "/health.json"))
ids = [b["id"] for b in h["backends"]]
assert {"mockgen", "mocksep"} <= set(ids), f"mock backends missing: {ids}"
print("health ok, backends =", sorted(ids))

g = wait(req("POST", "/jobs", {"backend": "mockgen", "task": "generate",
                               "params": {"prompt": "smoke test", "duration_s": 1.0, "seed": 7}})["job_id"])
assert g["state"] == "done", f"generate failed: {g.get('error')}"
arts = g["result"]["artifacts"]
assert all(pathlib.Path(p).is_file() and pathlib.Path(p).stat().st_size > 100 for p in arts.values()), arts
print("generate ok ->", sorted(arts))

s = wait(req("POST", "/jobs", {"backend": "mocksep", "task": "separate",
                               "params": {"audio_path": tmp + "/in.wav"}})["job_id"])
assert s["state"] == "done", f"separate failed: {s.get('error')}"
sarts = s["result"]["artifacts"]
assert {"vocals", "drums", "bass", "other"} <= set(sarts), sarts
prov = json.loads(pathlib.Path(s["result"]["provenance_file"]).read_text())
assert prov["backend_id"] == "mocksep" and len(prov["request_hash"]) == 64, prov
print("separate ok ->", sorted(sarts), "| provenance verified")

try:
    req("POST", "/jobs", {"backend": "nope", "task": "generate", "params": {}})
    raise SystemExit("FAIL: unknown backend accepted")
except urllib.error.HTTPError as e:
    body = json.loads(e.read().decode())
    assert e.code == 404 and "unknown_backend" in json.dumps(body), (e.code, body)
    print("error path ok: unknown backend ->", e.code)

print("SIDECAR SMOKE OK")
PYEOF
exit $?
