# `opd loudness` — true peak (dBTP), momentary/short-term max and loudness range (spec)

Status: normative (WP-0168). A beat is "export-ready" only if it does not clip between samples and sits at a
sane loudness. `opd analyze` (WP-0144) gives sample peak and integrated LUFS; this module adds the mastering
check numbers: **true peak**, the momentary / short-term maximum, and EBU Tech 3342 **loudness range**.
Everything is deterministic: same file in → byte-identical text out.

## Files
- `src/engine/loudness.hpp`, `src/engine/loudness.cpp` (new; registered in the `opengine` library sources in
  `CMakeLists.txt`).
- `CMakeLists.txt`: additive registration — `target_sources(opengine PRIVATE src/engine/loudness.cpp)`, the
  `op_loudness_tests` executable + `add_test(NAME loudness ...)`, and `list(APPEND DZ_OUR_TARGETS
  op_loudness_tests)` so `-Werror` and the sanitizer flags reach it.
- `tests/test_loudness.cpp`: standalone test binary (own `main`, own `CHECK` macro, exit code = failures).
- `src/cli/main.cpp`: subcommand `loudness` (+ usage line).

## API (exactly this, namespace `op::loudness`)
```cpp
#pragma once
#include <optional>
#include <string>
#include <vector>
namespace op::loudness {
struct Result {
    int sample_rate = 0;
    int channels = 0;                          // 1 or 2
    long long frames = 0;                      // samples per channel
    double duration_s = 0.0;                   // frames / sample_rate
    std::optional<double> true_peak_dbtp;
    std::optional<double> momentary_max_lufs;
    std::optional<double> short_term_max_lufs;
    std::optional<double> loudness_range_lu;
};
// interleaved float samples (non-finite samples are treated as 0.0). channels in {1,2}.
Result measure(const std::vector<float>& interleaved, int channels, int sample_rate);
std::string to_json(const Result& r);          // canonical text below, ending in "}\n"
}
```
`measure` with `channels` not in {1,2}, `sample_rate <= 0` or zero frames returns a `Result` with only
`sample_rate`/`channels`/`frames`/`duration_s` filled and every optional empty (with an invalid channel count
neither frames nor duration is computable, so both stay 0).

## Definitions (all arithmetic in `double`; read samples as the `float` values, converted to double)
- **true_peak_dbtp** — 4x oversampling per channel, then `20·log10` of the largest absolute value over ALL
  original samples and ALL interpolated samples of all channels; empty if that maximum is exactly 0.
  Interpolated samples sit at fractional offsets 1/4, 2/4, 3/4 between sample `i` and `i+1`
  (for `i = 0 .. frames-2`):

  ```
  y = Σ_{k=-11}^{12} x[i+k] · h(k − ph/4)      for ph = 1, 2, 3
  x[j] = 0 for j outside [0, frames)
  h(t) = sinc(t) · (0.5 + 0.5·cos(π·t/12))     for |t| < 12, else h(t) = 0
  sinc(0) = 1;  sinc(t) = sin(π·t)/(π·t);  π = op::dsp::kPi_d()
  ```

  (A full-scale sine at `fs/4` with phase `π/4` has sample peak −3.01 dBFS and must report a true peak within
  0.3 dB of 0 dBTP.)
- **K-weighting** — exactly the same filter as `opd analyze`: `loudness.cpp` declares
  `namespace op::analyze { void k_weight_coefficients(int fs, double out[10]); }` (defined in `analyze.cpp`:
  `out[0..4]` = stage 1 `b0,b1,b2,a1,a2`, `out[5..9]` = stage 2) and runs each channel through stage 1 then
  stage 2, Direct Form I, zero initial state:
  `y[n] = b0·x[n] + b1·x[n−1] + b2·x[n−2] − a1·y[n−1] − a2·y[n−2]`.
  Per-frame power `p[i] = Σ_c y_c[i]²` (weights 1.0). Prefix sums `P[0] = 0`, `P[i+1] = P[i] + p[i]`; the mean
  power of a window `[s, s+len)` is `(P[s+len] − P[s]) / len`. Loudness of a mean power `z`:
  `L(z) = −0.691 + 10·log10(z)`.
- Let `fs` = sample rate, `hop = (fs + 5) / 10` (integer division). Only **complete** windows count
  (`s + len <= frames`), window starts `s = 0, step, 2·step, ...`.
- **momentary_max_lufs** — windows `len = (4·fs + 5)/10`, `step = hop`, ungated: `L(max z)`. Empty when there
  is no complete window or the max `z` is 0.
- **short_term_max_lufs** — windows `len = 3·fs`, `step = hop`, ungated: `L(max z)`. Same empty rule.
- **loudness_range_lu** (EBU Tech 3342) — windows `len = 3·fs`, `step = fs`. Keep windows with `z > 0` and
  `L(z) > −70` (absolute gate). If none are kept → empty. `gate = L(mean of the kept z) − 20`; keep windows
  with `L(z) > gate`; if none remain → empty. Sort the remaining loudness values ascending (`std::sort` on
  doubles); percentile `q` uses `pos = q·(n−1)`, `lo = (size_t)pos`, `frac = pos − lo`,
  value = `v[lo] + frac·(v[lo+1] − v[lo])` (or `v[lo]` when `lo+1 >= n`). `LRA = pct(0.95) − pct(0.10)`. With a
  single remaining value `LRA = 0`.

## Output format (byte-exact; `to_json`)
```
{
  "sample_rate": 48000,
  "channels": 2,
  "frames": 480000,
  "duration_s": 10.000000,
  "true_peak_dbtp": -1.2345,
  "momentary_max_lufs": -14.0000,
  "short_term_max_lufs": -14.1000,
  "loudness_range_lu": 3.2000
}
```
Keys in exactly this order, 2-space indent, `\n` line endings, one trailing `\n` after `}`. Integers
(`sample_rate`, `channels`, `frames`) printed plainly; `duration_s` with `%.6f`; every other number with
`%.4f` (via `snprintf`; if the result is `-0.0000` print `0.0000`); an empty optional prints the bare token
`null` (as in the `opd analyze` JSON). No file path appears in the output.

## CLI
`opd loudness <in.wav> [--json]` — reads the file with `op::wav::read_audio` (WAV or AIFF/AIFF-C, chosen by
file signature), prints `to_json(measure(...))` to stdout and nothing else. `--json` is accepted and ignored
(JSON is the only output). Errors print nothing on stdout and use the existing `fail(...)` helper:

| case | exit | stderr |
| --- | --- | --- |
| no / extra / unknown argument | 2 | usage + `error: usage: ...` |
| file does not exist | 3 | `error: not_found: <path>` |
| unreadable audio (`read_audio` fails) | 4 | `error: parse: <reader error>` |
| sample rate outside [8000, 192000] | 5 | `error: loudness: unsupported sample rate <N>` |

`usage()` carries the line
`  opd loudness <in.wav> [--json]   (true peak dBTP, momentary/short-term max LUFS, loudness range as JSON)`.

## Non-goals
Gating/limiting, a UI meter, ITU 48-tap true-peak coefficients, and folding true peak into `opd analyze`
output are out of scope (see TASKS/WP-0168-loudness-true-peak.md).
