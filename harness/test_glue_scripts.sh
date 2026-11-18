#!/usr/bin/env bash
# test_glue_scripts.sh — CI smoke for the three glue scripts (WP-0134): stem/midi/prompt run
# end-to-end through harness/job_client.py against the sidecar's MOCK backends (no ML models,
# no GPU, no network). Contract under test: exit codes (0 happy path, 3 backend-gate, 2 usage),
# landed artifacts, and a bit-identical .opj from repeated runs. Output style follows
# harness/test_cli_errors.sh. Requires build/opd (render/attach/import-midi/midi2opj/info).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# Force the mock path even on a dev machine where real backends are configured.
unset OP_TRANSCRIBE_PYTHON OP_DEMUCS_PYTHON OP_GEN_BACKEND OP_STEM_BACKEND OP_TRANSCRIBE_BACKEND
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0

check() { # name expected_code cmd...
  local name="$1" want="$2"; shift 2
  local out got
  out="$("$@" 2>&1)"; got=$?
  if [ "$got" = "$want" ]; then echo "PASS $name (exit $got)"
  else echo "FAIL $name: want $want got $got"; printf '%s\n' "$out" | sed 's/^/    /'; fails=$((fails+1)); fi
}

exists() { # name path... — every listed path must exist (landed artifacts are the contract)
  local name="$1"; shift
  local p missing=""
  for p in "$@"; do [ -e "$p" ] || missing="$missing $p"; done
  if [ -z "$missing" ]; then echo "PASS $name"; else echo "FAIL $name: missing$missing"; fails=$((fails+1)); fi
}

fresh_proj() { # dest_dir — demo.opj + kits/ (the project refs kits/*.wav relative to itself)
  mkdir -p "$1"
  cp "$ROOT/tests/fixtures/goldens/demo.opj" "$1/"
  cp -R "$ROOT/tests/fixtures/goldens/kits" "$1/"
}

GL="$ROOT/harness"
PITCHED="$ROOT/tests/fixtures/goldens/pitched.wav"

# --- stem_into_project.sh: mocksep stems land in <projdir>/stems/ and get attached ---
fresh_proj "$TMP/stem"
check stem_exit0 0 bash "$GL/stem_into_project.sh" "$TMP/stem/demo.opj"
got="$(cd "$TMP/stem/stems" 2>/dev/null && ls | sort | tr '\n' ' ')"
want="bass.wav drums.wav other.wav vocals.wav "   # exact mocksep/demucs stem set, trailing space from tr
if [ "$got" = "$want" ]; then echo "PASS stem_files (exactly: $want)";
else echo "FAIL stem_files: want [$want] got [$got]"; fails=$((fails+1)); fi

# --- midi_into_project.sh: mocktrans MIDI lands in <projdir>/ai/ and notes merge into the opj ---
fresh_proj "$TMP/midi"
check midi_exit0 0 bash "$GL/midi_into_project.sh" "$TMP/midi/demo.opj" "$PITCHED"
exists midi_artifact "$TMP/midi/ai/transcribed.mid"

# --- prompt_to_song.sh: mockgen+mocktrans chain lands an editable song + final render ---
check prompt_exit0 0 bash "$GL/prompt_to_song.sh" "ci smoke" "$TMP/song"
exists prompt_artifacts "$TMP/song/final_render.wav" "$TMP/song/song.opj"

# --- -h on all three (WP-0133 convention) ---
check help_stem   0 bash "$GL/stem_into_project.sh" -h
check help_midi   0 bash "$GL/midi_into_project.sh" -h
check help_prompt 0 bash "$GL/prompt_to_song.sh" -h

# --- gate convention: backend requested without its runtime → exit 3 (OP_DEMUCS_PYTHON unset above) ---
fresh_proj "$TMP/gate"
check demucs_gate 3 env OP_STEM_BACKEND=demucs bash "$GL/stem_into_project.sh" "$TMP/gate/demo.opj"

# --- usage error: midi_into_project.sh with no args → exit 2 ---
check midi_usage 2 bash "$GL/midi_into_project.sh"

# --- determinism: same inputs, fresh copies → byte-identical resulting .opj ---
fresh_proj "$TMP/det1"; fresh_proj "$TMP/det2"
check det_run1 0 bash "$GL/midi_into_project.sh" "$TMP/det1/demo.opj" "$PITCHED"
check det_run2 0 bash "$GL/midi_into_project.sh" "$TMP/det2/demo.opj" "$PITCHED"
if cmp -s "$TMP/det1/demo.opj" "$TMP/det2/demo.opj"; then echo "PASS determinism (opj byte-identical)";
else echo "FAIL determinism: the two demo.opj runs differ"; fails=$((fails+1)); fi

# --- relative paths (WP-0140): resolved against the CALLER's cwd, artifacts land beside the project,
#     and nothing appears inside the repo (the sidecar runs in ai-sidecar/, so a path passed through
#     unresolved would be read — or, pre-WP-0133, written — there) ---
repo_before="$(git -C "$ROOT" status --porcelain 2>/dev/null)"
fresh_proj "$TMP/rel_stem"; cp "$PITCHED" "$TMP/rel_stem/"
check rel_stem_exit0 0 env -C "$TMP/rel_stem" bash "$GL/stem_into_project.sh" demo.opj pitched.wav
exists rel_stem_artifacts "$TMP/rel_stem/stems/vocals.wav" "$TMP/rel_stem/stems/drums.wav"
fresh_proj "$TMP/rel_midi"; cp "$PITCHED" "$TMP/rel_midi/"
check rel_midi_exit0 0 env -C "$TMP/rel_midi" bash "$GL/midi_into_project.sh" demo.opj pitched.wav
exists rel_midi_artifact "$TMP/rel_midi/ai/transcribed.mid"
mkdir -p "$TMP/rel_prompt"
check rel_prompt_exit0 0 env -C "$TMP/rel_prompt" bash "$GL/prompt_to_song.sh" "rel smoke" songout
exists rel_prompt_artifacts "$TMP/rel_prompt/songout/song.opj" "$TMP/rel_prompt/songout/final_render.wav"
if [ "$(git -C "$ROOT" status --porcelain 2>/dev/null)" = "$repo_before" ]; then echo "PASS rel_no_repo_strays";
else echo "FAIL rel_no_repo_strays: relative-path runs left files in the repo"; fails=$((fails+1)); fi

echo "test_glue_scripts: $fails failures"
exit $((fails > 0 ? 1 : 0))
