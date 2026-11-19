#!/usr/bin/env bash
# test_cli_errors.sh — verify opd exit codes match spec/project-model.md (failure paths are contracts too).
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

check usage_no_args        2 "$OPD"
check usage_bad_subcommand 2 "$OPD" frobnicate
check not_found_project    3 "$OPD" render "$TMP/nope.opj" -o "$TMP/o.wav"
printf 'this is not json {{{' > "$TMP/bad.opj"
check parse_error          4 "$OPD" render "$TMP/bad.opj" -o "$TMP/o.wav"
printf '{"format_version": 99}' > "$TMP/ver.opj"
check bad_format_version   4 "$OPD" render "$TMP/ver.opj" -o "$TMP/o.wav"
# sampler pointing at missing file -> render error (5)
cat > "$TMP/nokick.opj" <<'EOF'
{"format_version":0,"bpm":120,"sample_rate":48000,"seed":1,"length_bars":1,"beats_per_bar":4,
 "tracks":[{"name":"K","voice":{"kind":"sampler","path":"kits/missing.wav"}}],
 "patterns":[{"track":0,"notes":[{"t":0,"dur":1}]}]}
EOF
check sampler_missing      5 "$OPD" render "$TMP/nokick.opj" -o "$TMP/o.wav"

# --- opd apply (WP-0135): journal CLI contract per docs/spec/operations.md §CLI
cp "$ROOT/tests/fixtures/goldens/demo.opj" "$TMP/ap.opj"
printf '{"op":"set_bpm","bpm":95.0}\n# comment line\n\n{"op":"rename_track","index":0,"name":"Tick"}\n' > "$TMP/ok.opjl"
check apply_ok             0 "$OPD" apply "$TMP/ap.opj" "$TMP/ok.opjl" -o "$TMP/ap_out.opj"
if [ -s "$TMP/ap_out.opj" ]; then echo "PASS apply_output_written"; else echo "FAIL apply_output_written"; fails=$((fails+1)); fi
printf 'this is not json {{{\n' > "$TMP/badjson.opjl"
check apply_bad_journal    4 "$OPD" apply "$TMP/ap.opj" "$TMP/badjson.opjl" -o "$TMP/never1.opj"
if [ ! -e "$TMP/never1.opj" ]; then echo "PASS apply_parse_no_output"; else echo "FAIL apply_parse_no_output"; fails=$((fails+1)); fi
printf '{"op":"set_bpm","bpm":95.0}\n{"op":"set_bpm","bpm":100.0}\n{"op":"remove_track","index":0}\n' > "$TMP/rej.opjl"
err="$("$OPD" apply "$TMP/ap.opj" "$TMP/rej.opjl" -o "$TMP/never2.opj" 2>&1 >/dev/null)"; got=$?
if [ "$got" = 5 ] && [[ "$err" == *"line 3:"* ]]; then echo "PASS apply_rejected_line3 (exit 5, stderr names line)"; else echo "FAIL apply_rejected_line3: exit $got stderr [$err]"; fails=$((fails+1)); fi
if [ ! -e "$TMP/never2.opj" ]; then echo "PASS apply_reject_output_not_created"; else echo "FAIL apply_reject_output_not_created"; fails=$((fails+1)); fi
# rejection on an existing output file must leave its bytes untouched
cp "$TMP/ap_out.opj" "$TMP/keep.opj"
"$OPD" apply "$TMP/ap.opj" "$TMP/rej.opjl" -o "$TMP/keep.opj" >/dev/null 2>&1; got=$?
if [ "$got" = 5 ] && cmp -s "$TMP/keep.opj" "$TMP/ap_out.opj"; then echo "PASS apply_reject_output_unmodified"; else echo "FAIL apply_reject_output_unmodified: exit $got"; fails=$((fails+1)); fi
check apply_missing_proj   3 "$OPD" apply "$TMP/nope.opj" "$TMP/ok.opjl"
check apply_missing_journal 3 "$OPD" apply "$TMP/ap.opj" "$TMP/nope.opjl"
check apply_no_args        2 "$OPD" apply
# determinism: same journal applied to fresh copies → cmp-identical .opj
printf '{"op":"set_bpm","bpm":133.7}\n{"op":"add_attachment","path":"stems/x.wav","kind":"stem","producer":"cli"}\n' > "$TMP/det.opjl"
"$OPD" apply "$TMP/ap.opj" "$TMP/det.opjl" -o "$TMP/det1.opj" >/dev/null 2>&1; r1=$?
"$OPD" apply "$TMP/ap.opj" "$TMP/det.opjl" -o "$TMP/det2.opj" >/dev/null 2>&1; r2=$?
if [ "$r1" = 0 ] && [ "$r2" = 0 ] && cmp -s "$TMP/det1.opj" "$TMP/det2.opj"; then echo "PASS apply_determinism (identical .opj)"; else echo "FAIL apply_determinism: r1=$r1 r2=$r2"; fails=$((fails+1)); fi

# --- opd analyze (WP-0144): docs/spec/analyze.md §CLI exit-code contract
check analyze_no_args       2 "$OPD" analyze
check analyze_unknown_flag  2 "$OPD" analyze --wat
check analyze_missing_file  3 "$OPD" analyze "$TMP/nope.wav"
printf 'this is not a wav at all' > "$TMP/bad.wav"
check analyze_garbage_wav   4 "$OPD" analyze "$TMP/bad.wav"
# nothing on stdout for errors (stderr carries the message)
out="$("$OPD" analyze "$TMP/nope.wav" 2>/dev/null)"; got=$?
if [ "$got" = 3 ] && [ -z "$out" ]; then echo "PASS analyze_err_stdout_empty (exit 3)"; else echo "FAIL analyze_err_stdout_empty: exit $got stdout [$out]"; fails=$((fails+1)); fi
# valid generated WAV -> exit 0 with JSON on stdout; two consecutive runs byte-identical
"$OPD" render "$ROOT/tests/fixtures/goldens/demo.opj" -o "$TMP/ok.wav" --seed 1 >/dev/null 2>&1
check analyze_ok            0 "$OPD" analyze "$TMP/ok.wav"
check analyze_ok_json_flag  0 "$OPD" analyze "$TMP/ok.wav" --json
"$OPD" analyze "$TMP/ok.wav" > "$TMP/a1.json" 2>/dev/null
"$OPD" analyze "$TMP/ok.wav" > "$TMP/a2.json" 2>/dev/null
if [ -s "$TMP/a1.json" ] && cmp -s "$TMP/a1.json" "$TMP/a2.json"; then echo "PASS analyze_determinism (byte-identical JSON)"; else echo "FAIL analyze_determinism"; fails=$((fails+1)); fi

echo "test_cli_errors: $fails failures"
exit $((fails > 0 ? 1 : 0))
