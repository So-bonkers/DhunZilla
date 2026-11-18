#!/usr/bin/env bash
# run_goldens.sh — render every goldens/*.opj twice, assert bit-exact determinism, then diff vs golden.
# Usage: harness/run_goldens.sh [path/to/opd]   (default build/opd)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPD="${1:-$ROOT/build/opd}"
GOLD="$ROOT/tests/fixtures/goldens"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$OPD" ]; then echo "FATAL opd not built: $OPD (run cmake --build build)"; exit 2; fi
shopt -s nullglob
fixtures=("$GOLD"/*.opj)
if [ ${#fixtures[@]} -eq 0 ]; then echo "FATAL no golden fixtures found in $GOLD"; exit 2; fi

fails=0
for opj in "${fixtures[@]}"; do
    name="$(basename "$opj" .opj)"
    golden="$GOLD/$name.wav"
    if [ ! -f "$golden" ]; then echo "FAIL $name: missing golden $name.wav"; fails=$((fails+1)); continue; fi
    if ! "$OPD" render "$opj" -o "$TMP/$name.1.wav" >/dev/null || \
       ! "$OPD" render "$opj" -o "$TMP/$name.2.wav" >/dev/null; then
        echo "FAIL $name: render error"; fails=$((fails+1)); continue
    fi
    if ! cmp -s "$TMP/$name.1.wav" "$TMP/$name.2.wav"; then
        echo "FAIL $name: NON-DETERMINISTIC (run-to-run diff)"; fails=$((fails+1)); continue
    fi
    out=$(python3 "$ROOT/harness/golden_diff.py" "$TMP/$name.1.wav" "$golden" --mode bitexact) || true
    case "$out" in
        RESULT\ pass*) echo "PASS $name: deterministic + golden match" ;;
        *)             echo "FAIL $name: golden mismatch — $out"; fails=$((fails+1)) ;;
    esac
done

# Negative control: the harness itself must reject a corrupted golden.
cp "$GOLD/demo.wav" "$TMP/corrupt.wav"
python3 -c "
import sys
b = bytearray(open('$TMP/corrupt.wav','rb').read()); b[1000] ^= 0xFF
open('$TMP/corrupt.wav','wb').write(bytes(b))"
if python3 "$ROOT/harness/golden_diff.py" "$GOLD/demo.wav" "$TMP/corrupt.wav" >/dev/null 2>&1; then
    echo "FAIL negative-control: harness ACCEPTED a corrupted golden!"; fails=$((fails+1))
else
    echo "PASS negative-control (corrupt golden rejected)"
fi

echo "run_goldens: ${#fixtures[@]} fixtures, $fails failures"
exit $((fails > 0 ? 1 : 0))
