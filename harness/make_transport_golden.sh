#!/usr/bin/env bash
# make_transport_golden.sh — regenerate the WP-0242 transport fixtures (task §"The fixture project").
# Recipe (normative): opd new 2 bars @120 → apply transport_3_4_tempo.opjl (3/4, length 4 bars) →
# stdlib python writes the tempo_map directly (no journal op can express one; see operations.md) →
# opd apply with an empty journal re-saves through the canonical writer → opd click renders the WAV
# whose SHA-256 becomes the golden digest. Writes EXACTLY transport_3_4_tempo.opj and
# transport_3_4_tempo.click.sha256 into out_dir and nothing else; all intermediates live in mktemp -d.
# Usage: bash harness/make_transport_golden.sh [path/to/opd] [out_dir]
#   opd     default build/opd
#   out_dir default tests/fixtures/transport
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPD="${1:-$ROOT/build/opd}"
OUT="${2:-$ROOT/tests/fixtures/transport}"
if [ ! -x "$OPD" ]; then echo "FATAL opd not built/executable: $OPD" >&2; exit 2; fi
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

# 1. Canonical new project: 2 bars @ 120 BPM.
"$OPD" new "$TMP/base.opj" --bars 2 --bpm 120 >/dev/null

# 2. The committed journal: 3/4 (normalises to beats_per_bar 3) and length_bars 4.0.
"$OPD" apply "$TMP/base.opj" "$ROOT/tests/fixtures/transport/transport_3_4_tempo.opjl" -o "$TMP/signed.opj" >/dev/null

# 3. Tempo map: 120 BPM from bar 0, dropping to 90 BPM at bar 2 (mid-song). No operation in
#    docs/spec/operations.md can express a tempo map, so this step writes JSON directly —
#    deliberately NOT a new engine op (see the WP-0242 handoff PROPOSE: note).
python3 - "$TMP/signed.opj" <<'PYEOF'
import json, sys
path = sys.argv[1]
proj = json.load(open(path))
proj["tempo_map"] = [{"bar": 0.0, "bpm": 120.0}, {"bar": 2.0, "bpm": 90.0}]
json.dump(proj, open(path, "w"), sort_keys=True)
PYEOF

# 4. Empty-journal apply: re-save through the canonical writer, so the committed .opj bytes are canonical.
: > "$TMP/empty.opjl"
"$OPD" apply "$TMP/signed.opj" "$TMP/empty.opjl" -o "$OUT/transport_3_4_tempo.opj" >/dev/null

# 5. Click golden: digest of the opd click WAV pins every byte (the WAV itself is not committed).
"$OPD" click "$OUT/transport_3_4_tempo.opj" -o "$TMP/click.wav" >/dev/null
sha256sum "$TMP/click.wav" | cut -d' ' -f1 > "$OUT/transport_3_4_tempo.click.sha256"

echo "make_transport_golden: wrote $OUT/transport_3_4_tempo.opj + transport_3_4_tempo.click.sha256"
