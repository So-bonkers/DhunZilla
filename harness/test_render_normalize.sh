#!/usr/bin/env bash
# test_render_normalize.sh — CLI contract for `opd render --normalize-lufs / --ceiling-dbfs`
# (docs/spec/normalize.md §opd render --normalize-lufs, WP-0189). Style of harness/test_cli_errors.sh.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPD="${1:-$ROOT/build/opd}"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
fails=0

check() { # name expected_code cmd...
  local name="$1" want="$2"; shift 2
  "$@" >/dev/null 2>&1; local got=$?
  if [ "$got" = "$want" ]; then echo "PASS $name (exit $got)"; else echo "FAIL $name: want $want got $got"; fails=$((fails+1)); fi
}

# usage_err name expected_last_stderr_line cmd... — exit 2 and the LAST stderr line is exactly the message
usage_err() {
  local name="$1" want="$2"; shift 2
  local err got
  err="$("$@" 2>&1 >/dev/null)"; got=$?
  if [ "$got" = 2 ] && [ "$(printf '%s\n' "$err" | tail -n 1)" = "$want" ]; then
    echo "PASS $name (exit 2, stderr message exact)"
  else
    echo "FAIL $name: exit $got last stderr line [$(printf '%s\n' "$err" | tail -n 1)] want [$want]"; fails=$((fails+1))
  fi
}

# near name got want tol — numeric |got-want| <= tol via awk
near() {
  local name="$1" got="$2" want="$3" tol="$4"
  if [ -n "$got" ] && awk -v a="$got" -v b="$want" -v t="$tol" 'BEGIN{d=a-b; if(d<0)d=-d; exit !(d<=t)}'; then
    echo "PASS $name ($got within $tol of $want)"
  else
    echo "FAIL $name: got [$got] want $want +/- $tol"; fails=$((fails+1))
  fi
}

lufs_of() { "$OPD" analyze "$1" | sed -n 's/.*"integrated_lufs": *\([-0-9.]*\).*/\1/p'; }
peak_of() { "$OPD" analyze "$1" | sed -n 's/.*"peak_dbfs": *\([-0-9.]*\).*/\1/p'; }

# demo project copied next to its kits (demo.opj samples are relative to the .opj)
cp "$ROOT/tests/fixtures/goldens/demo.opj" "$TMP/demo.opj"
cp -R "$ROOT/tests/fixtures/goldens/kits" "$TMP/kits"

# --- the five ordered usage errors (exact stderr message, exit 2). All use the same never-written paths. ---
usage_err usage_bad_lufs "error: usage: bad --normalize-lufs value 'abc'" \
  "$OPD" render "$TMP/demo.opj" -o "$TMP/never.wav" --stems-dir "$TMP/never_stems" --normalize-lufs abc
usage_err usage_lufs_range  "error: usage: --normalize-lufs must be in [-70, 0]" \
  "$OPD" render "$TMP/demo.opj" -o "$TMP/never.wav" --stems-dir "$TMP/never_stems" --normalize-lufs 5
usage_err usage_bad_ceiling  "error: usage: bad --ceiling-dbfs value 'x'" \
  "$OPD" render "$TMP/demo.opj" -o "$TMP/never.wav" --stems-dir "$TMP/never_stems" --normalize-lufs -16 --ceiling-dbfs x
usage_err usage_ceiling_range "error: usage: --ceiling-dbfs must be in [-30, 0]" \
  "$OPD" render "$TMP/demo.opj" -o "$TMP/never.wav" --stems-dir "$TMP/never_stems" --normalize-lufs -16 --ceiling-dbfs -40
usage_err usage_ceiling_alone "error: usage: --ceiling-dbfs requires --normalize-lufs" \
  "$OPD" render "$TMP/demo.opj" -o "$TMP/never.wav" --stems-dir "$TMP/never_stems" --ceiling-dbfs -6
if [ ! -e "$TMP/never.wav" ] && [ ! -e "$TMP/never_stems" ]; then
  echo "PASS usage_errors_nothing_written"; else echo "FAIL usage_errors_nothing_written: output or stems dir exists"; fails=$((fails+1))
fi
# detected BEFORE the project load: non-existent project + bad value -> exit 2 (usage), not 3 (not_found)
check usage_before_load        2 "$OPD" render "$TMP/nope.opj" -o "$TMP/never2.wav" --normalize-lufs abc

# --- happy path: lands within 0.05 LU of the target; stdout is exactly two lines (rendered + JSON) ---
"$OPD" render "$TMP/demo.opj" -o "$TMP/norm.wav" --seed 1 --normalize-lufs -16 > "$TMP/norm_stdout.txt" 2>/dev/null
check happy_exit               0 test -s "$TMP/norm.wav"
near  happy_lufs "$(lufs_of "$TMP/norm.wav")" -16 0.05
if [ "$(wc -l < "$TMP/norm_stdout.txt")" = 2 ]; then echo "PASS stdout_two_lines_with_flag"; else echo "FAIL stdout_two_lines_with_flag: $(wc -l < "$TMP/norm_stdout.txt") lines"; fails=$((fails+1)); fi
"$OPD" render "$TMP/demo.opj" -o "$TMP/plain.wav" --seed 1 > "$TMP/plain_stdout.txt" 2>/dev/null
if [ "$(wc -l < "$TMP/plain_stdout.txt")" = 1 ]; then echo "PASS stdout_one_line_without_flag"; else echo "FAIL stdout_one_line_without_flag: $(wc -l < "$TMP/plain_stdout.txt") lines"; fails=$((fails+1)); fi

# --- ceiling-limited case: last stdout line ends '"limited":true}', written peak within 0.01 of -3 dBFS ---
"$OPD" render "$TMP/demo.opj" -o "$TMP/lim.wav" --seed 1 --normalize-lufs -8 --ceiling-dbfs -3 > "$TMP/lim_stdout.txt" 2>/dev/null
if tail -n 1 "$TMP/lim_stdout.txt" | grep -q '"limited":true}$'; then echo "PASS limited_flag_true"; else echo "FAIL limited_flag_true: [$(tail -n 1 "$TMP/lim_stdout.txt")]"; fails=$((fails+1)); fi
near  limited_peak "$(peak_of "$TMP/lim.wav")" -3 0.01

# --- stems written with --stems-dir (mix + one wav per track) ---
"$OPD" render "$TMP/demo.opj" -o "$TMP/mix_n.wav" --seed 1 --normalize-lufs -16 --stems-dir "$TMP/stems_out" >/dev/null 2>&1
n_stems=$(find "$TMP/stems_out" -maxdepth 1 -name '*.wav' 2>/dev/null | wc -l)
if [ -s "$TMP/mix_n.wav" ] && [ "$n_stems" -ge 1 ]; then echo "PASS stems_written ($n_stems stem wavs)"; else echo "FAIL stems_written: mix=$([ -s "$TMP/mix_n.wav" ] && echo yes || echo no) stems=$n_stems"; fails=$((fails+1)); fi

# --- silent project (empty pattern): exit 5 with the normalize message, nothing written ---
cat > "$TMP/silent.opj" <<'EOF'
{"format_version":0,"bpm":120,"sample_rate":48000,"seed":1,"length_bars":1,"beats_per_bar":4,
 "tracks":[{"name":"S","voice":{"kind":"sine","freq_hz":220}}],
 "patterns":[{"track":0,"notes":[]}]}
EOF
err="$("$OPD" render "$TMP/silent.opj" -o "$TMP/sil.wav" --stems-dir "$TMP/sil_stems" --normalize-lufs -16 2>&1 >/dev/null)"; got=$?
if [ "$got" = 5 ] && [[ "$err" == *"no measurable loudness"* ]]; then echo "PASS silent_exit5_message (exit 5, stderr has message)"; else echo "FAIL silent_exit5_message: exit $got stderr [$err]"; fails=$((fails+1)); fi
if [ ! -e "$TMP/sil.wav" ] && [ ! -e "$TMP/sil_stems" ]; then echo "PASS silent_nothing_written"; else echo "FAIL silent_nothing_written: output or stems dir exists"; fails=$((fails+1)); fi

# --- f32 equivalence: one-step render == render --format f32 then opd normalize (byte-identical files,
#     and the JSON stdout line is byte-identical to opd normalize's stdout) ---
"$OPD" render "$TMP/demo.opj" -o "$TMP/f_one.wav" --seed 1 --format f32 --normalize-lufs -16 > "$TMP/f_one_stdout.txt" 2>/dev/null
"$OPD" render "$TMP/demo.opj" -o "$TMP/f_raw.wav" --seed 1 --format f32 >/dev/null 2>&1
"$OPD" normalize "$TMP/f_raw.wav" -o "$TMP/f_two.wav" --lufs -16 --format f32 > "$TMP/f_two_stdout.txt" 2>/dev/null
if cmp -s "$TMP/f_one.wav" "$TMP/f_two.wav"; then echo "PASS f32_pipeline_byte_identical"; else echo "FAIL f32_pipeline_byte_identical"; fails=$((fails+1)); fi
tail -n 1 "$TMP/f_one_stdout.txt" > "$TMP/f_one_json.txt"
if cmp -s "$TMP/f_one_json.txt" "$TMP/f_two_stdout.txt"; then echo "PASS f32_stdout_json_line_identical"; else echo "FAIL f32_stdout_json_line_identical"; fails=$((fails+1)); fi

if [ "$fails" = 0 ]; then echo "render-normalize: ALL PASS"; exit 0; else echo "render-normalize: $fails FAILED"; exit 1; fi
