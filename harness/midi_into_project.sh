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
case "${1:-}" in -h|--help) sed -n '2,8p' "$0"; exit 0;; esac
PROJ="${1:-}"; AUDIO="${2:-}"
[ -f "$PROJ" ] || { echo "usage: $0 <proj.opj> <audio.wav>   (-h for details)"; exit 2; }
[ -f "$AUDIO" ] || { echo "FAIL: audio not found: $AUDIO"; exit 2; }
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ -n "${OP_TRANSCRIBE_PYTHON:-}" ]; then DEFAULT_BACKEND=basicpitch; else DEFAULT_BACKEND=mocktrans; fi
BACKEND="${OP_TRANSCRIBE_BACKEND:-$DEFAULT_BACKEND}"
if [ "$BACKEND" = "basicpitch" ] && [ -z "${OP_TRANSCRIBE_PYTHON:-}" ]; then
    echo "SKIP: basicpitch backend requested but OP_TRANSCRIBE_PYTHON not set"; exit 3
fi

OP_ROOT="$ROOT" python3 - "$PROJ" "$AUDIO" "$BACKEND" "$TMP" <<'PYEOF'
import os, pathlib, shutil, subprocess, sys

sys.path.insert(0, os.environ["OP_ROOT"] + "/harness")
from job_client import provenance_producer, start_sidecar, submit, wait_done

proj, audio, backend, tmp = sys.argv[1:5]
_op, base = start_sidecar(os.environ["OP_ROOT"], tmp + "/jobs")
snap = wait_done(base, submit(base, backend, "transcribe", {"audio_path": audio}))

producer = provenance_producer(snap, backend)
src_midi = pathlib.Path(snap["result"]["artifacts"]["midi"])

rel = "ai/transcribed.mid"
dst = pathlib.Path(proj).parent / rel
dst.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(src_midi, dst)

opd = os.environ["OP_ROOT"] + "/build/opd"
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
