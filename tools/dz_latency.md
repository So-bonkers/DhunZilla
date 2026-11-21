# dz_latency

WP-0190: the Phase 0 latency / xrun report for the JUCE-free `dz::Player` core (`src/app/player.hpp`,
library target `dzplayer`). It renders a project offline exactly like `opd render` without options, hands
the result to `dz::Player`, drives `process()` from simulated device callbacks of `--block` frames, times
every callback against the block budget, and prints one JSON report.

usage: dz_latency PROJECT.opj [--block N] [--budget-ms F] [--reference REF.wav]

Exit codes: 0 ok, 1 mismatch, 2 usage, 3 input, 5 render

## What it measures

At 48 kHz with 128-sample blocks the budget is `128 / 48000 s = 2.666667 ms`. Each `player.process()` call
is timed with `std::chrono::steady_clock` (two `now()` calls around the call itself; copying the callback's
output out of the arrays happens outside the timed region, and only the first `total_frames` frames are
kept). Over the per-block times the report gives:

- `max_ms`, `p99_ms`, `p50_ms` — the largest time and the nearest-rank percentiles
  (`sorted[ceil(q * n) - 1]` on the ascending-sorted times, no interpolation);
- `overruns` — blocks whose time is **strictly** greater than `budget_ms`.

It is a REPORT, not a CI gate: overruns never change the exit code. Wall-clock timing lives only in this
tool — never in `src/engine/`, `src/app/` or any model/player path (ADR-0004 determinism).

The same run also proves determinism: the audio the callbacks produce must equal what the offline renderer
produced, frame for frame (`output_identical`). With `--reference REF.wav` — an `opd render PROJECT -o REF.wav`
output — the callback audio is written as PCM16 to a pid-unique file in the system temp directory (removed
on every path) and compared byte for byte (`reference_match`).

## Options

| Option | Default | Meaning |
| --- | --- | --- |
| `--block N` | `128` | simulated callback size in frames; decimal digits `0-9` only, value in `[1, 16384]` (leading zeros allowed, so `0128` is 128; `+8`, `-1`, `1e3`, `8.0`, `abc`, `128abc` and the empty string are errors) |
| `--budget-ms F` | `block * 1000.0 / sample_rate` | budget override; must be fully consumed by `std::strtod` (`1e3` ok, `128abc` error), finite (`nan`, `inf` errors) and `> 0` |
| `--reference REF.wav` | none | WAV to byte-compare the callback output against |

Options may come in any order; if one is repeated the **last** occurrence wins (`--block 64 --block 128`
uses 128). Exactly one positional PROJECT.

## The report (byte-exact layout: 2-space indent, fixed key order, ends with `}\n`)

```json
{
  "project": "tests/fixtures/goldens/arranged.opj",
  "sample_rate": 48000,
  "block_frames": 128,
  "blocks": 3750,
  "total_frames": 480000,
  "budget_ms": 2.666667,
  "max_ms": 0.031472,
  "p99_ms": 0.000480,
  "p50_ms": 0.000080,
  "overruns": 0,
  "output_identical": true,
  "reference_match": true
}
```

`project` is the PROJECT argument as given (a standard JSON string). `budget_ms`, `max_ms`, `p99_ms` and
`p50_ms` are printed with `%.6f`; the timing values vary run to run, everything else is deterministic.
`reference_match` is the bare word `null` when `--reference` was not given.

## Errors (nothing on stdout for exit 2 and 3)

| Case | stderr | exit |
| --- | --- | --- |
| no PROJECT / a second positional / unknown option / missing option value / bad `--block` / bad `--budget-ms` | `error: <reason>` then the `usage:` line | 2 |
| PROJECT does not exist | `error: project not found: <path>` | 3 |
| `op::load_project` fails | `error: <loader message>` | 3 |
| `--reference` file cannot be opened | `error: cannot read reference: <path>` | 3 |
| render fails or produces zero frames, or the temp WAV cannot be written | `error: render failed: <reason>` | 5 |
| `output_identical` false, or `reference_match` false | (the report is still printed) | 1 |

## Running it

The standard load project for the Phase 0 number is `tests/fixtures/goldens/arranged.opj` (4 bars,
3 tracks, 480000 frames at 48 kHz = 3750 blocks of 128):

```sh
cmake -S . -B build && cmake --build build -j4
build/opd render tests/fixtures/goldens/arranged.opj -o /tmp/ref.wav
build/dz_latency tests/fixtures/goldens/arranged.opj --reference /tmp/ref.wav
```

The binary is `<build>/dz_latency`. It is deliberately not registered with `add_test`: it is a report,
not a gate.

## Tests

`tools/test_dz_latency.py` (stdlib `unittest`) runs only when all three env vars are set:

```sh
DZ_LATENCY_BIN=build/dz_latency DZ_OPD_BIN=build/opd \
DZ_PROJECT=tests/fixtures/goldens/arranged.opj python3 -m unittest -v tools/test_dz_latency.py
```
