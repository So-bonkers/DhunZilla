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
case "${1:-}" in -h|--help) sed -n '2,10p' "$0"; exit 0;; esac
PROJ="${1:-}"
[ -f "$PROJ" ] || { echo "usage: $0 <proj.opj> [audio.wav]   (-h for details)"; exit 2; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

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

OP_ROOT="$ROOT" python3 - "$PROJ" "$AUDIO" "$BACKEND" "$TMP" <<'PYEOF'
import os, pathlib, subprocess, sys

sys.path.insert(0, os.environ["OP_ROOT"] + "/harness")
from job_client import provenance_producer, start_sidecar, submit, wait_done

proj, audio, backend, tmp = sys.argv[1:5]
_op, base = start_sidecar(os.environ["OP_ROOT"], tmp + "/jobs")
snap = wait_done(base, submit(base, backend, "separate", {"audio_path": audio}))

producer = provenance_producer(base, snap, backend)
artifacts = dict(sorted(snap["result"]["artifacts"].items()))
assert artifacts, "no artifacts in result"

opd = os.environ["OP_ROOT"] + "/build/opd"
n = 0
for name, src in artifacts.items():
    rel = f"stems/{name}.wav"
    dst = pathlib.Path(proj).parent / rel
    dst.parent.mkdir(parents=True, exist_ok=True)
    import shutil
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
