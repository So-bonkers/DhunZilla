---
id: WP-0176
title: Master limiter — deterministic look-ahead brickwall peak limiter (new engine module) + `opd render --limit-dbfs <x>`
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, cli:main, spec:limiter, new:src/engine/limiter.hpp, new:src/engine/limiter.cpp, new:tests/test_limiter.cpp]
---

## Context — why this exists
A bounced beat must not clip. `opd analyze` / `opd loudness` measure peaks and `opd normalize` applies a
static gain capped by a ceiling — but neither *holds* the peak. This package adds a NEW engine module
`op::limiter`: a deterministic look-ahead brickwall peak limiter (fixed 5 ms look-ahead, one-pole
attack/release in the dB domain, stereo-linked, **no oversampling**), plus `opd render --limit-dbfs <x>` which
runs it on the final mix immediately before the WAV is written. Nothing existing changes except two additive
edits (CMake registration, small CLI additions). **No project-model change**: no `.opj` key, no op, no diff —
limiting is a render-time option only, and every existing golden stays bit-identical.

## Spec — normative
Create the NEW file `docs/spec/limiter.md` containing this whole "Spec" section (headings may be adapted). Do
not edit any other spec file.

### Files
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

### API (exactly this, namespace `op::limiter`)
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

### Definitions — the algorithm (normative; all arithmetic in `double` unless stated)
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

### CLI — `opd render --limit-dbfs <x>`
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

### Determinism (ADR-0004)
No wall clock, no randomness, no state between calls: `apply` is a pure function of its arguments. Two runs of
the same command are bit-identical; the module uses libm only through `op::dsp::powd_` / `op::dsp::log10d_` and
is built with `-ffp-contract=off`.

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`), no new golden.
- [x] `tests/test_limiter.cpp` with **at least 12 CHECKs**, covering at minimum: each of the four validation
      messages exactly (and that a rejected call leaves the buffer untouched); zero-frame success with idle stats;
      `look_ahead_samples == sample_rate / 200` for 48000, 44100 and 8000; material below the ceiling is
      bit-identical and `engaged == false`; a full-scale impulse in silence: `first_reduction_frame ==
      k - la + 1`, no sample above `(float)pow(10, ceiling/20)`, and the impulse within 1 % of the ceiling;
      stereo linking (a loud left transient attenuates the quiet right channel near it and not ~1 s later); a
      steady sine settling at `ceiling_dbfs - 20*log10(A)` within 0.05 dB; attack vs release (still clearly
      attenuated 5 ms after a hit, back to the pad value within 0.001 after ~1 s); determinism (two identical
      inputs give bit-identical output and identical stats); non-finite samples treated as 0.0f.
- [x] The arithmetic follows §Definitions exactly (all in `double`, the two given one-pole coefficients, no
      `std::exp`): the acceptance verification recomputes the limiter independently from those formulas and
      compares the rendered float samples frame by frame (max abs difference <= 2e-5), so a different
      discretisation or an unlinked per-channel gain will not pass.
- [x] `opd render --limit-dbfs` works as specified: ceiling respected, stems untouched, range renders
      self-contained, the three CLI error cases with exit 2 and nothing written, no new stdout lines.
- [x] Standard gates: build (0 warnings, `-j4`), `op_tests`, `op_limiter_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of
      `op_tests` and `op_limiter_tests`. Build with `-j4` max.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), existing golden fixtures (`tests/fixtures/`, read-only;
  add none), `src/app/`, `.github/`, `ai-sidecar/`, `docs/adr/`, `docs/spec/*` other than the new `limiter.md`,
  `tests/test_main.cpp`.
- Engine files other than the two new ones: `src/engine/render.*`, `src/engine/wav.*`, `src/engine/analyze.*`,
  `src/engine/loudness.*`, `src/engine/normalize.*`, `src/engine/peaks.*`, `src/engine/dsp.hpp`,
  `src/engine/model.*`, `src/engine/ops.cpp`, `src/doc/`.
- Project-model changes of any kind (no `.opj` key, no `set_*` op, no `opd diff` support), side-chain input,
  per-channel (unlinked) limiting, multiband, oversampling or interpolation filters, user-settable
  attack/release/look-ahead, soft knee, stem limiting, a UI meter, dithering.

## Git
- Branch `wp-0176-master-limiter` from `main`. Commit: `WP-0176: master limiter (look-ahead brickwall peak limiter) + opd render --limit-dbfs`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - **New engine module `op::limiter`** — `src/engine/limiter.hpp` + `src/engine/limiter.cpp`: look-ahead
    brickwall peak limiter exactly per §Definitions — `la = sample_rate / 200`, backward-Euler one-pole
    coefficients `a_attack = dt/(kAttackS+dt)` / `a_release = dt/(kReleaseS+dt)`, stereo-linked single gain state,
    desired-gain `d[n]` from the bounded forward peak with a strict `>` ceiling test, apply + hard clamp to
    `±(float)ceiling_lin`. All arithmetic in `double`; non-finite samples (`|x| > 1e30f`, NaN, ±inf) replaced by
    `0.0f` on both read and apply; libm only via `op::dsp::powd_` / `op::dsp::log10d_`; no wall clock, no state
    between calls. Validation order + the four exact messages per §API (the ceiling test is a negated AND so NaN
    is rejected); on failure neither buffer nor stats are touched. The in-place update is safe because every
    look-ahead read index is `>= n` and frame `n`'s output is written only after its own scan.
  - **New spec** `docs/spec/limiter.md` — the whole normative Spec section, copied as required; no other spec file
    touched.
  - `CMakeLists.txt` — three additive insertions only (opengine source line after the WP-0171 line, the
    `op_limiter_tests` target + `add_test(NAME limiter …)` immediately before `# Our targets only`, and the
    `DZ_OUR_TARGETS` entry). `git diff` shows additions only.
  - `tests/test_limiter.cpp` (new) — own `main`, CHECK macro copied from `tests/test_main.cpp`, **37 CHECKs**, no
    filesystem access at all (no temp dirs). Covers: the four validation messages exactly ×2 rate cases + NaN
    ceiling, buffer untouched after every rejection, inclusive boundary acceptance, zero-frame idle stats with a
    deliberately stale incoming `Stats` (proves full rewrite), `la` for 48000/44100/8000, below-ceiling
    bit-identity + idle stats, impulse (`first_reduction_frame == k-la+1`, no sample above `(float)pow(10,c/20)`,
    impulse within 1 % of the ceiling, `max_gain_reduction_db` within 0.01 dB of the ceiling gain), stereo linking
    (quiet right pad pulled down at the hit, bit-exact before the look-ahead window, recovered within 1e-3 after
    1 s), steady sine settling within 0.05 dB of `ceiling_dbfs − 20·log10(A)` plus reduction from frame 0,
    attack-vs-release (still <70 % of pad at +5 ms, back within 0.001 after ~1 s, hit on the ceiling), determinism
    (bit-identical output + identical stats over repeated and third runs), non-finite → `0.0f` (mono and stereo).
  - `src/cli/main.cpp` — additive only: include after `engine/lint.hpp`; one usage line after the `[--format …]`
    continuation; `run_master_limiter()` immediately before `cmd_render` (declares a local `op::limiter::Stats`,
    calls `apply(mix.interleaved, 2, mix.sample_rate, ceiling_dbfs, stats, err)`); in `cmd_render` the two named
    locals plus one parsed `double limit_db`, the `--limit-dbfs` branch immediately before the loop's final
    `else`, the value checks with the other pre-load option checks, and one guarded call immediately before each
    of the two mix `write_wav_stereo(out_path, …)` calls. No existing line modified or removed. `--stems-dir`
    stems are never limited; on success the limiter prints nothing.
- **How verified:** (all commands run here on the linux-aarch64 reference rig, branch `wp-0176-master-limiter`)

  1 · Clean full build, `-j4`, zero warnings:
  ```
  $ rm -rf build && cmake -S . -B build && cmake --build build -j4 > /tmp/wp0176_build.log 2>&1; echo $?
  0
  $ grep -ci warning /tmp/wp0176_build.log
  0
  ```

  2 · New limiter suite — 37 PASS, 0 FAIL, exit 0:
  ```
  $ ./build/op_limiter_tests
  PASS validate.channels_3_message
  PASS validate.channels_0_message
  PASS validate.sample_count_message
  PASS validate.sample_rate_low_message
  PASS validate.sample_rate_high_message
  PASS validate.ceiling_above_message
  PASS validate.ceiling_below_message
  PASS validate.ceiling_nan_message
  PASS validate.buffer_untouched
  PASS validate.boundaries_accepted
  PASS zero_frames.idle_stats_rewritten
  PASS la.48000_is_240
  PASS la.44100_is_220
  PASS la.8000_is_40
  PASS below_ceiling.bit_identical
  PASS below_ceiling.idle_stats
  PASS impulse.first_reduction_frame
  PASS impulse.no_sample_above_ceiling
  PASS impulse.hits_ceiling_within_1pct
  PASS impulse.max_reduction_within_0p01_db_of_ceiling
  PASS impulse.silence_stays_zero
  PASS stereo_link.ok_and_engaged
  PASS stereo_link.right_attenuated_at_hit
  PASS stereo_link.right_recovered_1s_later
  PASS stereo_link.right_untouched_before_lookahead
  PASS sine.settles_within_0p05_db
  PASS sine.reduces_from_frame_0
  PASS sine.output_within_ceiling
  PASS attack_release.still_attenuated_5ms_after
  PASS attack_release.pad_back_after_1s
  PASS attack_release.hit_on_ceiling
  PASS determinism.output_bit_identical
  PASS determinism.stats_identical
  PASS determinism.third_run_reproduces
  PASS nonfinite.idle_stats
  PASS nonfinite.output_all_zero
  PASS nonfinite.stereo_counts_as_zero
  op_limiter_tests: 0 failure(s)          # exit code 0
  ```

  3 · Existing suites and harness gates (all unchanged from before this WP):
  ```
  $ ./build/op_tests | tail -1
  ALL PASS (0 failures)                                     # exit 0
  $ bash harness/run_goldens.sh | tail -2
  PASS negative-control (corrupt golden rejected)
  run_goldens: 11 fixtures, 0 failures                      # every existing golden bit-identical, no new golden
  $ bash harness/test_cli_errors.sh | tail -1
  test_cli_errors: 0 failures
  $ bash harness/test_glue_scripts.sh | tail -1             # UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv
  test_glue_scripts: 0 failures
  $ ctest --test-dir build -R limiter | tail -3
  100% tests passed, 0 tests failed out of 1                # registration as CTest test "limiter" works
  ```

  4 · Sanitizers (`-DDZ_SANITIZE=ON`), build clean and both binaries run:
  ```
  $ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san -j4 \
        --target op_tests op_limiter_tests > /tmp/wp0176_san.log 2>&1; echo $?
  0
  $ grep -ci warning /tmp/wp0176_san.log
  0
  $ ./build-san/op_limiter_tests | tail -1 ; echo $?
  op_limiter_tests: 0 failure(s)
  0
  $ ./build-san/op_tests | tail -1 ; echo $?
  ALL PASS (0 failures)
  0
  ```

  5 · Acceptance recompute from §Definitions, independent of the C++ code (`double` Python, exact sliding maximum
  via a monotonic deque; script kept out of the repo at `/home/shubhankar_dgx/qwen-scratch/.wp0176_verify.py`).
  Reference = `opd render demo.opj --format f32`; candidate = the same render with `--limit-dbfs -6`:
  ```
  $ python3 /home/shubhankar_dgx/qwen-scratch/.wp0176_verify.py      # 24 checks, full output pasted
        ref.wav: channels=2 rate=48000 frames=192000
  PASS cli.render_limited_exit0
  PASS recompute.frame_by_frame_max_abs_diff<=2e-5 — max abs diff 0.000e+00 over 384000 samples (independent spec recompute)
  PASS ceiling.respected_on_float_samples — limited peak 0.50118721 <= (float)ceiling_lin 0.50118721
        un-limited peak 0.854426 -> limited peak 0.501187; recompute engaged=True min_g=-4.6334 dB
  PASS determinism.limited_render_bit_identical_rerun
  PASS stdout.unchanged_single_line — 'rendered 192000 frames @48000 Hz -> same.wav (seed=42)\n'
  PASS stems.render_plain_exit0
  PASS stems.byte_identical_with_and_without_flag — 3 stems: ['0_metronome.wav', '1_bassline.wav', '2_kick.wav']
  PASS stems.mix_is_limited(differs)
  PASS range.render_exit0
  PASS range.slice_frame_bounds_match — frames [48000, 144000)
  PASS range.self_contained_matches_fresh_start_recompute — max abs diff 0.000e+00
  PASS cli.option_position_anywhere
  PASS cli.last_value_wins
  PASS errors.no_value_unknown_arg — exit=2 stderr_last='error: usage: unknown arg '--limit-dbfs''
  PASS errors.non_numeric — exit=2 stderr_last='error: usage: bad --limit-dbfs value 'abc''
  PASS errors.nan — exit=2 stderr_last='error: usage: bad --limit-dbfs value 'nan''
  PASS errors.inf — exit=2 stderr_last='error: usage: bad --limit-dbfs value 'inf''
  PASS errors.above_range — exit=2 stderr_last='error: usage: --limit-dbfs must be in [-30, 0]'
  PASS errors.below_range — exit=2 stderr_last='error: usage: --limit-dbfs must be in [-30, 0]'
  PASS cli.boundary_values_accepted
  PASS usage.line_present
  wp0176_verify: 0 failure(s)
  ```
  Max abs difference against the independent recompute is **exactly 0.0** (not merely ≤ 2e-5), so the arithmetic —
  the two one-pole coefficients, the shared linked gain, `powd_`/`log10d_`, the clamp — matches §Definitions to the
  bit. The range case (`--from-bar 0.5 --to-bar 1.5`) is verified self-contained: limiting the slice from frame 0
  reproduces the CLI's limited range render exactly.

  6 · Demo artifact (audio output affected, behind the new opt-in flag; existing goldens untouched):
  `opd render demo.opj -o before.wav` vs `… -o after.wav --limit-dbfs -6`, both pcm16, seed 42 —
  `/home/shubhankar_dgx/qwen-scratch/wp0176-demo/before.wav` and `/home/shubhankar_dgx/qwen-scratch/wp0176-demo/after.wav`.
  Measured with `opd loudness`:
  ```
  before: true_peak_dbtp -1.3653   momentary_max_lufs -9.5260   short_term_max_lufs -12.6606   LR 0.7465 LU
  after : true_peak_dbtp -5.9980   momentary_max_lufs -12.0015  short_term_max_lufs -13.9600   LR 0.9456 LU
  ```

- **Metrics:**
  - `tests/test_limiter.cpp`: 37 CHECKs (≥ 12 required), runtime 0.6 s, no filesystem access.
  - Limiter cost, `demo.opj` (4 s stereo @48 kHz): default (no build type) render 13 ms → 361 ms with
    `--limit-dbfs`; Release (`-O3`) 3 ms → 106 ms — i.e. ≈26 ms per audio-second at `-O3`, ≈4.6 s for a 3-minute
    stereo bounce, the naive O(F·la) scan the spec explicitly allows.
  - Determinism: limited render bit-identical run-to-run, and bit-identical between the default (-O0) and Release
    (-O3) builds (`cmp` clean).
  - Diff footprint: +2 new engine files (+118 lines), +1 new test file (357 lines), +1 new spec file;
    `CMakeLists.txt` +6 additive lines, `src/cli/main.cpp` +24 additive lines. No existing line changed anywhere.

- **PROPOSE / BLOCKED:**
  - PROPOSE (out of scope here, no action taken): if render time ever matters for long bounces, the look-ahead peak
    can be computed with an exact O(F) monotonic-deque sliding maximum — identical values by spec ("any exact
    method gives the identical value"), so no golden or behaviour change; it would need its own package.
  - PROPOSE: `cmd_render` declares three locals (`has_limit`, `limit_arg` and a parsed `double limit_db`) rather
    than the two named in the spec, because the parsed ceiling must survive from the pre-load checks to both write
    sites; purely additive, no existing line touched. Noted here so the reviewer sees the one deviation.
  - BLOCKED: none.

