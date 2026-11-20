# Master limiter — deterministic look-ahead brickwall peak limiter (spec)

Status: normative (WP-0176). A bounced beat must not clip. `opd analyze` / `opd loudness` measure peaks and
`opd normalize` applies a static gain capped by a ceiling — but neither *holds* the peak. This module adds a NEW
engine module `op::limiter`: a deterministic look-ahead brickwall peak limiter (fixed 5 ms look-ahead, one-pole
attack/release in the dB domain, stereo-linked, **no oversampling**), plus `opd render --limit-dbfs <x>` which
runs it on the final mix immediately before the WAV is written. Nothing existing changes except two additive
edits (CMake registration, small CLI additions). **No project-model change**: no `.opj` key, no op, no diff —
limiting is a render-time option only, and every existing golden stays bit-identical.

## Files
- `src/engine/limiter.hpp`, `src/engine/limiter.cpp` (new).
- `CMakeLists.txt`: purely ADDITIVE edits (do not change or delete any existing line):
  1. directly after the line `target_sources(opengine PRIVATE src/engine/peaks.cpp)  # WP-0171` add
     `target_sources(opengine PRIVATE src/engine/limiter.cpp)  # WP-0176`;
  2. immediately before the comment line starting `# Our targets only` add:
     `add_executable(op_limiter_tests tests/test_limiter.cpp)`,
     `target_link_libraries(op_limiter_tests PRIVATE opengine)`,
     `add_test(NAME limiter COMMAND op_limiter_tests)`;
  3. directly after the line `list(APPEND DZ_OUR_TARGETS op_peaks_tests)  # WP-0171` add
     `list(APPEND DZ_OUR_TARGETS op_limiter_tests)  # WP-0176`.
- `tests/test_limiter.cpp` (new; do NOT edit `tests/test_main.cpp`): its own `main`, a local
  `CHECK(cond, name)` macro printing `PASS name` / `FAIL name` (copy the style from `tests/test_main.cpp`),
  returns the failure count. It needs no filesystem access (no temp dirs at all).
- `src/cli/main.cpp`: additive edits only, no existing line modified or removed:
  1. `#include "engine/limiter.hpp"` directly after the `#include "engine/lint.hpp"` line;
  2. in `usage()`, one new line immediately after the `"             [--format pcm16|pcm24|f32]\n"` render
     continuation line, exactly:
     `             [--limit-dbfs X]   (master brickwall limiter on the final mix, docs/spec/limiter.md)`;
  3. a helper `static bool run_master_limiter(op::wav::StereoBuffer& mix, double ceiling_dbfs, std::string& err)`
     immediately before `static int cmd_render`; it declares a local `op::limiter::Stats`, calls
     `op::limiter::apply(mix.interleaved, 2, mix.sample_rate, ceiling_dbfs, stats, err)` and returns the result;
  4. in `cmd_render`: two new locals (`bool has_limit = false;` and `std::string limit_arg;`), one new
     `else if` branch for `--limit-dbfs` in the option loop immediately before that loop's final
     `else { usage(); return fail("usage", std::string("unknown arg '") + argv[i] + "'"); }`, storing the raw
     argument into `limit_arg` and setting `has_limit = true`; the value checks together with the other
     pre-load option checks; and one guarded call to `run_master_limiter` immediately before **each** existing
     `op::wav::write_wav_stereo(out_path, ...)` for the mix (both the plain branch and the `--stems-dir`
     branch), e.g. `if (has_limit && !run_master_limiter(buf, limit_db, err)) return fail("render", err);`.
- Do NOT edit `src/engine/render.*`, `src/engine/wav.*`, `src/engine/analyze.*`, `src/engine/dsp.hpp`,
  `src/engine/model.*`, `src/engine/ops.cpp`, `src/doc/` or any other engine file. libm only via the existing
  `op::dsp::` wrappers (`powd_`, `log10d_`); no direct `std::pow/log10/sqrt/exp/fabs/...` in `limiter.cpp`
  (`+ - * / < > ==`, casts and `std::to_string` are fine; absolute value is a ternary, not `std::fabs`).

## API (exactly this, namespace `op::limiter`)
```cpp
#pragma once
#include <string>
#include <vector>
namespace op::limiter {
constexpr double kLookAheadS = 0.005;  // fixed look-ahead: reduction starts this far before a peak
constexpr double kAttackS = 0.0005;    // one-pole time constant while the gain is falling
constexpr double kReleaseS = 0.050;    // one-pole time constant while the gain is rising
struct Stats {
    long long frames = 0;                  // frames processed (samples per channel)
    int sample_rate = 0;                   // sample rate used for the time constants
    int look_ahead_samples = 0;            // la = sample_rate / 200 (integer division)
    bool engaged = false;                  // true iff some frame applied a gain below 0 dB
    double max_gain_reduction_db = 0.0;    // most negative smoothed gain over all frames (<= 0)
    long long first_reduction_frame = -1;  // first frame with a smoothed gain below 0 dB, else -1
};
bool apply(std::vector<float>& interleaved, int channels, int sample_rate, double ceiling_dbfs, Stats& stats,
           std::string& err);
}
```

`apply` validates in this order and returns false with exactly this `err` (the buffer is left untouched):
1. `channels` not 1 or 2: `limiter: channels must be 1 or 2`
2. `interleaved.size()` not a multiple of `channels`: `limiter: sample count not a multiple of channels`
3. `sample_rate` outside `[8000, 192000]`: `limiter: unsupported sample rate <N>` (`<N>` = the decimal rate)
4. `ceiling_dbfs` not in `[-30, 0]`, NaN included (NaN fails every comparison): `limiter: ceiling out of range [-30, 0]`

On success `stats` is fully rewritten (never left stale). Zero frames is a success with an idle `Stats`
(`frames = 0`, `engaged = false`, `max_gain_reduction_db = 0.0`, `first_reduction_frame = -1`,
`look_ahead_samples = la`).

## Definitions — the algorithm (normative; all arithmetic in `double` unless stated)
`F` = frames (`interleaved.size() / channels`), `C` = `channels`, samples interleaved (`L[,R]`).
**A sample not in `[-1e30f, 1e30f]` (which includes NaN and ±inf) is replaced by `0.0f`** before use — both
when measuring and when applying the gain; a range test suffices because every comparison with NaN is false.

Constants and derived values:
```
la          = sample_rate / 200                    // integer division: 5 ms (48000 -> 240, 44100 -> 220, 8000 -> 40)
dt          = 1.0 / (double)sample_rate
a_attack    = dt / (kAttackS + dt)                 // backward-Euler one-pole coefficient, in (0, 1)
a_release   = dt / (kReleaseS + dt)                // backward-Euler one-pole coefficient, in (0, 1)
ceiling_lin = op::dsp::powd_(10.0, ceiling_dbfs / 20.0)
```
Do **not** use `std::exp` or another discretisation; these two coefficients are the specified ones.

Per frame `n` (`0 <= n < F`):
1. **Stereo-linked instantaneous peak**: `m[n] = max over c of |x[n*C + c]|` — the maximum of the float
   magnitudes, kept in a `double`.
2. **Look-ahead peak** — bounded forward maximum, clamped at the end:
   `p[n] = max(m[n .. min(n + la - 1, F - 1)])`. Compute it however you like; a straightforward scan of up to
   `la` frames per frame is fine (about 1 s for a 3-minute stereo bounce at `-O2`). Any exact method gives the
   identical value.
3. **Desired gain in dB** (strict comparison — material exactly at the ceiling is not over it):
   `d[n] = (p[n] > ceiling_lin) ? ceiling_dbfs - 20.0 * op::dsp::log10d_(p[n]) : 0.0`.
4. **Smoothed gain** `g`, one state shared by all channels, initial value `g = 0.0` (unity):
   ```
   if (d[n] < g) g = g + a_attack  * (d[n] - g);    // falling: fast
   else          g = g + a_release * (d[n] - g);     // rising: slow
   ```
5. **Apply and clamp** — `gl = op::dsp::powd_(10.0, g / 20.0)`, then for every channel
   `y = static_cast<float>((double)x[n*C + c] * gl)`, then with `cl = (float)ceiling_lin`:
   `if (y > cl) y = cl; else if (y < -cl) y = -cl;`. The written sample is `y`.

Consequences that follow from the rules above (informative, and what the unit tests check):
- A single full-scale impulse at frame `k` in silence first reduces gain at frame `max(0, k - la + 1)` and the
  impulse lands on the ceiling; no sample anywhere exceeds it.
- If no frame exceeds the ceiling, `d[n] == 0.0` everywhere, `g` stays exactly `0.0`, `gl == 1.0`, every output
  sample is **bit-identical** to the input and `engaged == false`.
- A steady sine of peak amplitude `A > ceiling_lin` settles at a smoothed gain of
  `ceiling_dbfs - 20*log10(A)` dB, which is what `max_gain_reduction_db` converges to.
- The ceiling is enforced on the float samples *before* WAV quantization: with `pcm16`/`pcm24` output the file's
  peak can sit up to one LSB above `ceiling_lin` (for pcm16, at most `+3.05e-5`). Do not "fix" that by
  pre-shrinking the ceiling.

## CLI — `opd render --limit-dbfs <x>`
- `<x>` is a finite decimal in `[-30, 0]` **dBFS**, parsed with the existing strict whole-argument helper
  `parse_bar_value` (so `abc`, `1x`, `nan`, `inf` are rejected). If it appears more than once the last value wins.
  Without the flag there is no limiting and `opd render` behaves exactly as before.
- Option position does not matter. The value checks run with the other option-syntax checks, i.e. **before** the
  project file is opened; nothing is written on any error. Errors (exit 2, usage to stderr, nothing on stdout):
  - `--limit-dbfs` without a value -> the existing unknown-argument path: `error: usage: unknown arg '--limit-dbfs'`;
  - non-numeric / non-finite -> `error: usage: bad --limit-dbfs value '<X as given>'` (the argument verbatim);
  - outside `[-30, 0]` -> `error: usage: --limit-dbfs must be in [-30, 0]`.
- Where it runs: on the **final mix only**, after any bar-range slicing (`--from-bar`/`--to-bar`/`--loop`) and
  immediately before `op::wav::write_wav_stereo(out_path, ...)`, with `channels = 2` and the buffer's own sample
  rate — so look-ahead state starts fresh at frame 0 of what is written. **Stems are never limited**:
  `--stems-dir` writes byte-identical stems with or without the flag.
- A failure from `op::limiter::apply` (only reachable when the project's sample rate is outside `[8000, 192000]`)
  uses the existing render error path: exit 5, `error: render: <err>`.
- On success stdout is unchanged from today: the `rendered ... -> <path> (seed=N)` line, plus the
  `stem i -> <path>` lines when `--stems-dir` is used. The limiter prints nothing of its own.

## Determinism (ADR-0004)
No wall clock, no randomness, no state between calls: `apply` is a pure function of its arguments. Two runs of
the same command are bit-identical; the module uses libm only through `op::dsp::powd_` / `op::dsp::log10d_` and
is built with `-ffp-contract=off`.
