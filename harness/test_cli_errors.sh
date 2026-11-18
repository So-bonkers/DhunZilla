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

echo "test_cli_errors: $fails failures"
exit $((fails > 0 ? 1 : 0))
