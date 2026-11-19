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

# --- opd render bar range (WP-0145): exit-code contract per docs/spec/project-model.md §Render a bar range
cp "$ROOT/tests/fixtures/goldens/demo.opj" "$TMP/rr.opj"
cp -R "$ROOT/tests/fixtures/goldens/kits" "$TMP/kits"   # demo.opj samples are relative to the .opj
check range_bad_value      2 "$OPD" render "$TMP/rr.opj" -o "$TMP/r1.wav" --from-bar abc
if [ ! -e "$TMP/r1.wav" ]; then echo "PASS range_bad_value_no_output"; else echo "FAIL range_bad_value_no_output"; fails=$((fails+1)); fi
check range_reversed       2 "$OPD" render "$TMP/rr.opj" -o "$TMP/r2.wav" --from-bar 2 --to-bar 1
if [ ! -e "$TMP/r2.wav" ]; then echo "PASS range_reversed_no_output"; else echo "FAIL range_reversed_no_output"; fails=$((fails+1)); fi
check range_too_big        2 "$OPD" render "$TMP/rr.opj" -o "$TMP/r3.wav" --to-bar 99
if [ ! -e "$TMP/r3.wav" ]; then echo "PASS range_too_big_no_output"; else echo "FAIL range_too_big_no_output"; fails=$((fails+1)); fi
check range_ok             0 "$OPD" render "$TMP/rr.opj" -o "$TMP/r4.wav" --from-bar 0.5 --to-bar 1.5
if [ -s "$TMP/r4.wav" ]; then echo "PASS range_ok_output_written"; else echo "FAIL range_ok_output_written"; fails=$((fails+1)); fi
# --- opd click (WP-0148): same exit-code contract as render (spec §Click track render)
check click_no_out         2 "$OPD" click "$ROOT/tests/fixtures/goldens/demo.opj"
check click_missing_proj   3 "$OPD" click "$TMP/nope.opj" -o "$TMP/c.wav"
check click_parse_error    4 "$OPD" click "$TMP/bad.opj" -o "$TMP/c.wav"
check click_ok             0 "$OPD" click "$ROOT/tests/fixtures/goldens/demo.opj" -o "$TMP/click.wav"
if [ -s "$TMP/click.wav" ]; then echo "PASS click_output_written"; else echo "FAIL click_output_written"; fails=$((fails+1)); fi
# --- opd render --format (WP-0149): invalid value → usage error before load, nothing written ---
check bad_format           2 "$OPD" render "$ROOT/tests/fixtures/goldens/demo.opj" -o "$TMP/o.wav" --format mp3
if [ ! -e "$TMP/o.wav" ]; then echo "PASS bad_format_no_output"; else echo "FAIL bad_format_no_output: file was written"; fails=$((fails+1)); fi

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
# --- opd diff (WP-0146): journal CLI contract per docs/spec/operations.md §opd diff
check diff_no_args         2 "$OPD" diff
check diff_missing_out     2 "$OPD" diff "$TMP/ap.opj" "$TMP/ap_out.opj"
check diff_not_found       3 "$OPD" diff "$TMP/nope.opj" "$TMP/ap_out.opj" -o "$TMP/d_nf.opjl"
check diff_identical_ok    0 "$OPD" diff "$ROOT/tests/fixtures/goldens/demo.opj" "$ROOT/tests/fixtures/goldens/demo.opj" -o "$TMP/d_id.opjl"
if [ -f "$TMP/d_id.opjl" ] && [ ! -s "$TMP/d_id.opjl" ]; then echo "PASS diff_identical_zero_bytes"; else echo "FAIL diff_identical_zero_bytes"; fails=$((fails+1)); fi
check diff_ok              0 "$OPD" diff "$TMP/ap.opj" "$TMP/ap_out.opj" -o "$TMP/d_ok.opjl"
"$OPD" apply "$TMP/ap.opj" "$TMP/d_ok.opjl" -o "$TMP/d_rt.opj" >/dev/null 2>&1
if cmp -s "$TMP/d_rt.opj" "$TMP/ap_out.opj"; then echo "PASS diff_roundtrip_byte_identical"; else echo "FAIL diff_roundtrip_byte_identical"; fails=$((fails+1)); fi
sed 's/"seed": 42/"seed": 43/' "$ROOT/tests/fixtures/goldens/demo.opj" > "$TMP/seed43.opj"
err="$("$OPD" diff "$ROOT/tests/fixtures/goldens/demo.opj" "$TMP/seed43.opj" -o "$TMP/never_d.opjl" 2>&1 >/dev/null)"; got=$?
if [ "$got" = 5 ] && [ "$err" = "error: diff: unsupported difference: seed" ]; then echo "PASS diff_unsupported (exit 5, stderr names field)"; else echo "FAIL diff_unsupported: exit $got stderr [$err]"; fails=$((fails+1)); fi
if [ ! -e "$TMP/never_d.opjl" ]; then echo "PASS diff_unsupported_no_output"; else echo "FAIL diff_unsupported_no_output"; fails=$((fails+1)); fi
sed 's/"start_bar": 2.0, "repeats": 1/"start_bar": 2.0, "repeats": 2/' "$ROOT/tests/fixtures/goldens/arranged.opj" > "$TMP/rep2.opj"
check diff_clip_repeats_ok  0 "$OPD" diff "$ROOT/tests/fixtures/goldens/arranged.opj" "$TMP/rep2.opj" -o "$TMP/d_rep.opjl"

# --- opd steps (WP-0154): docs/spec/operations.md §Engine view and CLI exit-code contract
cat > "$TMP/st_base.opj" <<'EOF'
{"format_version":0,"bpm":120,"sample_rate":48000,"seed":1,"length_bars":1,"beats_per_bar":4,
 "tracks":[{"name":"K","voice":{"kind":"sine","freq_hz":220}}],
 "patterns":[{"track":0,"notes":[]}]}
EOF
check steps_bad_index      2 "$OPD" steps "$TMP/st_base.opj" x
check steps_steps_zero     2 "$OPD" steps "$TMP/st_base.opj" 0 --steps 0
check steps_missing_file   3 "$OPD" steps "$TMP/nope.opj" 0
check steps_index_range    2 "$OPD" steps "$TMP/st_base.opj" 1
printf '{"op":"set_step","pattern":0,"step":0,"on":true}\n{"op":"set_step","pattern":0,"step":4,"on":true}\n{"op":"set_step","pattern":0,"step":8,"on":true}\n{"op":"set_step","pattern":0,"step":12,"on":true}\n' > "$TMP/st.opjl"
"$OPD" apply "$TMP/st_base.opj" "$TMP/st.opjl" -o "$TMP/st.opj" >/dev/null 2>&1
out="$("$OPD" steps "$TMP/st.opj" 0 2>/dev/null)"; got=$?
if [ "$got" = 0 ] && [ "$out" = "x...x...x...x..." ]; then echo "PASS steps_ok (exit 0, x...x...x...x...)"; else echo "FAIL steps_ok: exit $got out [$out]"; fails=$((fails+1)); fi

# --- opd render --loop / opd info loop (WP-0155): docs/spec/project-model.md §CLI additions
cp "$ROOT/tests/fixtures/goldens/demo.opj" "$TMP/lp_noloop.opj"
cp -R "$ROOT/tests/fixtures/goldens/kits" "$TMP/kits"   # demo.opj samples are relative to the .opj
check loop_no_loop_usage   2 "$OPD" render "$TMP/lp_noloop.opj" -o "$TMP/lp0.wav" --loop
if [ ! -e "$TMP/lp0.wav" ]; then echo "PASS loop_no_loop_no_output"; else echo "FAIL loop_no_loop_no_output"; fails=$((fails+1)); fi
check loop_with_from       2 "$OPD" render "$TMP/lp_noloop.opj" -o "$TMP/lp1.wav" --loop --from-bar 0
check loop_with_to         2 "$OPD" render "$TMP/lp_noloop.opj" -o "$TMP/lp2.wav" --to-bar 1 --loop
if [ ! -e "$TMP/lp1.wav" ] && [ ! -e "$TMP/lp2.wav" ]; then echo "PASS loop_combined_no_output"; else echo "FAIL loop_combined_no_output"; fails=$((fails+1)); fi
printf '{"op":"set_loop","start_bar":0.5,"end_bar":1.5}\n' > "$TMP/setloop.opjl"
"$OPD" apply "$TMP/lp_noloop.opj" "$TMP/setloop.opjl" -o "$TMP/lp.opj" >/dev/null 2>&1; got=$?
if [ "$got" = 0 ]; then echo "PASS loop_apply_set (exit 0)"; else echo "FAIL loop_apply_set: exit $got"; fails=$((fails+1)); fi
check loop_ok              0 "$OPD" render "$TMP/lp.opj" -o "$TMP/lp_loop.wav" --loop
"$OPD" render "$TMP/lp.opj" -o "$TMP/lp_range.wav" --from-bar 0.5 --to-bar 1.5 >/dev/null 2>&1
if cmp -s "$TMP/lp_loop.wav" "$TMP/lp_range.wav"; then echo "PASS loop_bytes_equal_from_to"; else echo "FAIL loop_bytes_equal_from_to"; fails=$((fails+1)); fi
"$OPD" render "$TMP/lp.opj" -o "$TMP/lp_full.wav" >/dev/null 2>&1
"$OPD" render "$TMP/lp_noloop.opj" -o "$TMP/lp_noloop.wav" >/dev/null 2>&1
if cmp -s "$TMP/lp_full.wav" "$TMP/lp_noloop.wav"; then echo "PASS loop_render_without_flag_unchanged"; else echo "FAIL loop_render_without_flag_unchanged"; fails=$((fails+1)); fi
out="$("$OPD" info "$TMP/lp.opj" | grep '^loop: ')"
if [ "$out" = "loop: 0.5-1.5" ]; then echo "PASS loop_info_line"; else echo "FAIL loop_info_line: [$out]"; fails=$((fails+1)); fi
out="$("$OPD" info "$TMP/lp_noloop.opj" | grep -c '^loop')"
if [ "$out" = 0 ]; then echo "PASS loop_info_absent"; else echo "FAIL loop_info_absent: [$out]"; fails=$((fails+1)); fi

# --- opd new (WP-0165): docs/spec/project-model.md §New project exit-code contract
check new_no_args          2 "$OPD" new
check new_bars_zero        2 "$OPD" new "$TMP/nv.opj" --bars 0
if [ ! -e "$TMP/nv.opj" ]; then echo "PASS new_bars_zero_no_output"; else echo "FAIL new_bars_zero_no_output: file was created"; fails=$((fails+1)); fi
check new_bad_track_kind   2 "$OPD" new "$TMP/nv.opj" --track D:drumkit
if [ ! -e "$TMP/nv.opj" ]; then echo "PASS new_bad_track_no_output"; else echo "FAIL new_bad_track_no_output: file was created"; fails=$((fails+1)); fi
check new_ok               0 "$OPD" new "$TMP/nw.opj"
if [ -s "$TMP/nw.opj" ]; then echo "PASS new_output_written"; else echo "FAIL new_output_written"; fails=$((fails+1)); fi
err="$("$OPD" new "$TMP/nw.opj" 2>&1 >/dev/null)"; got=$?
if [ "$got" = 5 ] && [ "$err" = "error: write: $TMP/nw.opj already exists (use --force)" ]; then echo "PASS new_exists_refused (exit 5, spec message)"; else echo "FAIL new_exists_refused: exit $got stderr [$err]"; fails=$((fails+1)); fi
check new_force            0 "$OPD" new "$TMP/nw.opj" --force
echo "test_cli_errors: $fails failures"
exit $((fails > 0 ? 1 : 0))
