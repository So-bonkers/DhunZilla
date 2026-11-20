---
id: WP-0168
title: `opd loudness` - true peak (dBTP), momentary/short-term max and loudness range (new engine module)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.2
human_review: none
---

## Context - why this exists
A beat is "export-ready" only if it does not clip between samples and sits at a sane loudness. `opd analyze`
(WP-0144) gives sample peak and integrated LUFS but not **true peak**, nor the short-term / range numbers a
mastering check needs. This package adds a NEW engine module `op::loudness` and an `opd loudness` command.
Nothing existing is modified except two small additive edits (CMake registration, one CLI dispatch block).

## Spec - normative
Create the NEW file `docs/spec/loudness.md` containing the content of this whole "Spec" section (headings may
be adapted). Do not edit other spec files.

### Files
- `src/engine/loudness.hpp`, `src/engine/loudness.cpp` (new).
- `CMakeLists.txt`: purely ADDITIVE edits (do not change or delete any existing line):
  1. directly after the line `target_include_directories(opengine PUBLIC src third_party)` add
     `target_sources(opengine PRIVATE src/engine/loudness.cpp)  # WP-0168`;
  2. immediately before the comment line starting `# Our targets only` add a new test executable block:
     `add_executable(op_loudness_tests tests/test_loudness.cpp)`,
     `target_link_libraries(op_loudness_tests PRIVATE opengine)`,
     `add_test(NAME loudness COMMAND op_loudness_tests)`;
  3. directly after the line `set(DZ_OUR_TARGETS opengine dzdoc opd op_tests)` add
     `list(APPEND DZ_OUR_TARGETS op_loudness_tests)  # WP-0168` (so `-Werror` and the sanitizer flags reach it).
- `tests/test_loudness.cpp` (new; do NOT edit `tests/test_main.cpp`). It has its own `main`, its own small
  `CHECK(cond, name)` macro (copy the macro style from `tests/test_main.cpp`: prints `PASS name` / `FAIL name`),
  and returns the failure count.
- `src/cli/main.cpp`: ONE small additive block - `#include "engine/loudness.hpp"` after the `engine/dsp.hpp`
  include; one usage line inserted immediately BEFORE the `opd steps` usage line; a `static int cmd_loudness(...)`
  function placed immediately BEFORE the comment of `parse_decimal` (the line starting `// Parse a decimal
  argument`); and one dispatch line `if (cmd == "loudness") return cmd_loudness(argc - 1, argv + 1);` placed
  immediately BEFORE the `if (cmd == "steps")` line. No existing line may be modified or removed.
- Do NOT edit `src/engine/analyze.*`, `src/engine/dsp.hpp` or any other engine file. libm only via the
  existing `op::dsp::` wrappers (`sind_`, `cosd_`, `log10d_`); no direct `std::sin/log10/...` calls in
  `loudness.cpp` (`+ - * / <` and `std::sort` are fine).

### API (exactly this, namespace `op::loudness`)
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
`measure` with channels not in {1,2}, sample_rate <= 0 or zero frames returns a Result with only
sample_rate/channels/frames/duration_s filled and every optional empty.

### Definitions (all arithmetic in `double`; read samples as the `float` values, converted to double)
- **true_peak_dbtp** - 4x oversampling per channel, then 20*log10 of the largest absolute value over ALL
  original samples and ALL interpolated samples of all channels; empty if that maximum is exactly 0.
  Interpolated samples sit at fractional offsets 1/4, 2/4, 3/4 between sample i and i+1 (for i = 0 .. frames-2):
  `y = sum_{k=-11}^{12} x[i+k] * h(k - ph/4)` for ph = 1, 2, 3, with `x[j] = 0` for j outside `[0, frames)` and
  `h(t) = sinc(t) * (0.5 + 0.5*cos(pi*t/12))` for |t| < 12, `h(t) = 0` otherwise, `sinc(0) = 1`,
  `sinc(t) = sin(pi*t)/(pi*t)`; `pi = op::dsp::kPi_d()`.
  (A full-scale sine at fs/4 with phase pi/4 has sample peak -3.01 dBFS and must report a true peak within
  0.3 dB of 0 dBTP.)
- **K-weighting** - use exactly the same filter as `opd analyze`: declare in `loudness.cpp`
  `namespace op::analyze { void k_weight_coefficients(int fs, double out[10]); }` (it already exists in
  `analyze.cpp`: `out[0..4]` = stage 1 `b0,b1,b2,a1,a2`, `out[5..9]` = stage 2), then run each channel through
  stage 1 then stage 2, Direct Form I, zero initial state:
  `y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2]`.
  Per-frame power `p[i] = sum over channels of y_c[i]^2` (weights 1.0). Build prefix sums
  `P[0] = 0, P[i+1] = P[i] + p[i]`; the mean power of a window `[s, s+len)` is `(P[s+len] - P[s]) / len`.
  Loudness of a mean power z: `L(z) = -0.691 + 10*log10(z)`.
- Let `fs` = sample rate, `hop = (fs + 5) / 10` (integer division). Only COMPLETE windows count
  (`s + len <= frames`), window starts `s = 0, step, 2*step, ...`.
- **momentary_max_lufs** - windows `len = (4*fs + 5) / 10`, `step = hop`, ungated: `L(max z)`. Empty when there
  is no complete window or the max z is 0.
- **short_term_max_lufs** - windows `len = 3*fs`, `step = hop`, ungated: `L(max z)`. Same empty rule.
- **loudness_range_lu** (EBU Tech 3342) - windows `len = 3*fs`, `step = fs`. Keep windows with `z > 0` and
  `L(z) > -70` (absolute gate). If none are kept -> empty. `gate = L(mean of the kept z) - 20`; keep windows
  with `L(z) > gate`; if none remain -> empty. Sort the remaining loudness values ascending (`std::sort` on
  doubles); percentile q uses `pos = q*(n-1)`, `lo = (size_t)pos`, `frac = pos - lo`,
  value = `v[lo] + frac*(v[lo+1]-v[lo])` (or `v[lo]` when `lo+1 >= n`). `LRA = pct(0.95) - pct(0.10)`.
  With a single remaining value LRA = 0.

### JSON (byte-exact; every number `%.4f` except duration `%.6f`; the text `-0.0000` is printed `0.0000`)
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
An empty optional prints the bare word `null` (as in the `opd analyze` JSON).

### CLI
`opd loudness <in.wav> [--json]` - reads the file with `op::wav::read_audio` (WAV or AIFF), prints
`to_json(measure(...))` to stdout and nothing else. `--json` is accepted and ignored. Errors print nothing on
stdout and use the existing `fail(...)` helper: no/extra/unknown argument -> exit 2 (`usage`); missing file ->
3 (`not_found`); unreadable audio -> 4 (`parse`, message = the reader's error); sample rate outside
[8000, 192000] -> 5 (code string `loudness`, message `unsupported sample rate <N>`).

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`).
- [x] `tests/test_loudness.cpp` with at least 8 CHECKs, covering: inter-sample peak (fs/4, pi/4 phase sine:
      true peak within 0.3 dB of 0, far above the sample peak), low-frequency sine true peak ~ sample peak
      (amplitude 0.5 -> -6.02 +/- 0.05), silence and empty input -> all optionals empty, stationary 997 Hz
      stereo tone 8 s: momentary max and short-term max within 0.05 LU of `analyze_samples(...).integrated_lufs`
      and `loudness_range_lu < 0.1`, input shorter than 400 ms -> no momentary/short-term/LRA but a true peak,
      two 12 s halves 10 dB apart -> LRA within 0.5 of 10, momentary max >= short-term max, `to_json` shape
      and determinism (two calls identical).
- [x] `opd loudness` works as specified; error exit codes as specified.
- [x] Standard gates: build (0 warnings, `-j4`), `op_tests`, `op_loudness_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of
      `op_tests` and `op_loudness_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `src/app/`, `.github/`, golden fixtures (`tests/fixtures/`),
  `docs/spec/` other than the new `loudness.md`, `docs/adr/`, `ai-sidecar/`, `tests/test_main.cpp`,
  `src/engine/analyze.*`, `src/engine/dsp.hpp`, `src/engine/model.*`, `src/engine/ops.cpp`, `src/doc/`.
- Folding true peak into `opd analyze` output, gating/limiting, a UI meter, ITU 48-tap true-peak coefficients.

## Git
- Branch `wp-0168-loudness-true-peak` from `main`. Commit: `WP-0168: opd loudness - true peak, momentary/short-term max, loudness range`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - **New** `src/engine/loudness.hpp` / `src/engine/loudness.cpp` — namespace `op::loudness`, API exactly as
    spec'd: `Result measure(...)`, `std::string to_json(const Result&)`. True peak = 4x oversampling with the
    24-tap raised-cosine-windowed sinc (`h(t) = sinc(t)·(0.5+0.5·cos(π·t/12))`, taps precomputed once per call
    — same argument values, so bit-identical to evaluating per sample); K-weighting reuses `analyze.cpp`'s
    `k_weight_coefficients(fs, out[10])` via an extern declaration (no copy of the filter), prefix-sum window
    means, ungated momentary / short-term maxima, EBU Tech 3342 LRA (−70 absolute gate, Γ = L(mean z) − 20,
    linear-interpolated P95 − P10). All arithmetic `double`; libm only through `op::dsp::` (`sind_`, `cosd_`,
    `log10d_`) — verified: zero direct `std::sin/cos/log/pow/sqrt` calls in the file.
  - **New** `tests/test_loudness.cpp` — own `main`, own `CHECK` macro (same style as `test_main.cpp`), returns
    the failure count; 15 CHECKs. `tests/test_main.cpp` untouched.
  - **New** `docs/spec/loudness.md` — the normative spec content from this work package (headings adapted); no
    other spec file touched.
  - `CMakeLists.txt` — purely additive, exactly the three edits listed in the spec (target_sources after the
    include-directories line; `op_loudness_tests` block immediately before `# Our targets only`;
    `list(APPEND DZ_OUR_TARGETS op_loudness_tests)` right after the `set(...)`). Diff: **+5 / −0**.
  - `src/cli/main.cpp` — purely additive single block: include after `engine/dsp.hpp`, usage line before the
    `opd steps` line, `static int cmd_loudness(...)` immediately before `// Parse a decimal argument`, dispatch
    line immediately before `if (cmd == "steps")`. Diff: **+29 / −0**. Uses `op::wav::read_audio` (WAV or AIFF).
  - Nothing else touched: no engine file other than the new one, no golden/fixture edit, no ROADMAP/.github/app
    change. `git diff --stat`: `CMakeLists.txt | 5 +++++`, `src/cli/main.cpp | 29 +++++++++++++++++++++++++++`
    (0 deleted lines in either).

- **How verified:** (all commands run on this aarch64 DGX Spark rig, branch `wp-0168-loudness-true-peak`)

  **1. Build — `-Wall -Wextra -Werror`, clean rebuild, `-j4`: 0 warnings**
  ```
  $ cmake --build build --clean-first -j4 ; echo exit=$?
  exit=0
  warning/error lines: 0
  [ 94%] Built target opd
  [100%] Linking CXX executable op_tests
  [100%] Built target op_tests
  ```

  **2. `./build/op_loudness_tests` — 15/15 PASS (exit 0, 0.34 s)**
  ```
  PASS loudness.tp_fs4_sine_sample_peak_is_minus3_01
  PASS loudness.tp_fs4_sine_reports_0_dbtp_far_above_sample_peak
  PASS loudness.tp_low_freq_sine_matches_sample_peak_minus6_02
  PASS loudness.silence_all_optionals_empty
  PASS loudness.empty_input_all_optionals_empty
  PASS loudness.bad_channels_or_rate_returns_optionals_empty
  PASS loudness.stationary_tone_momentary_and_short_term_match_analyze_lufs
  PASS loudness.stationary_tone_loudness_range_below_0_1lu
  PASS loudness.input_shorter_than_400ms_only_true_peak
  PASS loudness.lra_two_halves_10db_apart_is_10lu
  PASS loudness.momentary_max_ge_short_term_max
  PASS loudness.mono_input_measured
  PASS loudness.to_json_matches_spec_sample_byte_exact
  PASS loudness.silence_json_prints_null
  PASS loudness.repeat_measure_is_byte_identical
  ALL PASS (0 failures)
  ```

  **3. `op_tests` + ctest (both registered tests)**
  ```
  $ ./build/op_tests | tail -1
  ALL PASS (0 failures)
  $ ctest --test-dir build
      Start 1: unit
  1/2 Test #1: unit .............................   Passed    0.53 sec
      Start 2: loudness
  2/2 Test #2: loudness .........................   Passed    0.35 sec

  100% tests passed, 0 tests failed out of 2
  ```

  **4. `harness/run_goldens.sh` — existing goldens bit-identical (nothing in the render path changed)**
  ```
  PASS tempo: deterministic + golden match
  PASS negative-control (corrupt golden rejected)
  run_goldens: 9 fixtures, 0 failures        # exit 0
  ```

  **5. `harness/test_cli_errors.sh` — unchanged suite still green** → `test_cli_errors: 0 failures` (exit 0;
  66 `PASS` lines, 0 `FAIL`).
  **6. `harness/test_glue_scripts.sh`** (with `UV_CACHE_DIR=<workspace>/.cache/uv`)
  → `test_glue_scripts: 0 failures` (exit 0; 21 `PASS` lines, 0 `FAIL`, incl. the `.opj` determinism checks).

  **7. Sanitizers (`-DDZ_SANITIZE=ON`, ASAN+UBSAN), both binaries built and run clean**
  ```
  $ cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON
  $ cmake --build build-san -j4 --target op_tests op_loudness_tests ; echo exit=$?   → exit=0
  $ ./build-san/op_loudness_tests | tail -1        → ALL PASS (0 failures)     (exit 0)
  $ ./build-san/op_tests | tail -1                 → ALL PASS (0 failures)     (exit 0, 8.1 s)
  # CLI path under ASAN (leak detection on), "ERROR: " occurrences per file: 0 / 0 / 0 / 0
  $ build-san/opd loudness demo.wav    → exit=0   bad.wav → exit=4   low.wav → exit=5   tone.aiff → exit=0
  ```

  **8. `opd loudness` CLI behaviour (real output, 48 kHz stereo golden render of `demo.opj`, seed 42)**
  ```
  $ opd loudness demo.wav                # exit 0, stdout only
  {
    "sample_rate": 48000,
    "channels": 2,
    "frames": 192000,
    "duration_s": 4.000000,
    "true_peak_dbtp": -1.3653,
    "momentary_max_lufs": -9.5260,
    "short_term_max_lufs": -12.6606,
    "loudness_range_lu": 0.7465
  }
  # cross-check vs the existing analyzer on the same file: peak_dbfs -1.3665 (sample) < true peak -1.3653 dBTP,
  # integrated_lufs -13.1453 sits below short_term_max_lufs -12.6606 — consistent.
  ```
  **Error contract** (each case: exit code asserted and stdout asserted empty; stderr shown):
  ```
  PASS no_args            (exit 2, stdout empty) | usage: …
  PASS unknown_flag       (exit 2, stdout empty) | usage: …        # opd loudness --wat
  PASS extra_arg          (exit 2, stdout empty) | usage: …        # two paths / stray arg
  PASS missing_file       (exit 3, stdout empty) | error: not_found: nope.wav
  PASS garbage_wav        (exit 4, stdout empty) | error: parse: file too small for canonical WAV
  PASS unsupported_rate   (exit 5, stdout empty) | error: loudness: unsupported sample rate 4000
  PASS determinism        (two runs byte-identical, 218 bytes)
  $ opd 2>&1 | grep loudness
    opd loudness <in.wav> [--json]   (true peak dBTP, momentary/short-term max LUFS, loudness range as JSON)
  ```
  AIFF path: `opd loudness tone.aiff` → exit 0, identical numbers to the same audio as WAV
  (`true_peak_dbtp -19.9989`, `momentary_max_lufs -19.9988`).

  **9. Sidecar gates (AGENTS.md CI list; `ai-sidecar/` untouched by this package, run anyway for completeness)**
  ```
  $ uv run --offline ruff check .   → All checks passed!                     (exit 0)
  $ uv run --offline pytest -q      → 109 passed, 2 skipped in 14.54s        (exit 0)
  ```

- **Metrics:** (exact values from the measured signals; the same numbers the CHECKs assert against)
  ```
  tone8s:     integrated=-20.0000 momentary=-19.9986 short=-20.0000 lra=0.000020 true_peak=-20.0000
  fs4:        sample_peak=-3.0103 dBFS  true_peak=+0.1043 dBTP  delta=3.115 dB
  step10db:   momentary=-19.9986 short=-20.0000 lra=10.000000 true_peak=-20.0000
  low100hz:   sample_peak=-6.0206 dBFS  true_peak=-6.0198 dBTP  (delta 0.0008 dB)
  ```
  - Inter-sample peak detection works: full-scale sine at fs/4, phase π/4 → sample peak −3.0103 dBFS but true
    peak **+0.1043 dBTP**, i.e. within 0.3 dB of 0 dBTP and **3.115 dB above** the sample peak (the classic
    "reads clean but clips between samples" case). The same signal rendered through the CLI as pcm16 gave
    +0.1042 dBTP vs `opd analyze` sample peak −3.0105 dBFS.
  - Low-frequency sine (100 Hz, amp 0.5): true peak −6.0198 vs sample peak −6.0206 dBFS — no inter-sample gain,
    both inside the −6.02 ± 0.05 band.
  - Stationary 997 Hz stereo tone, 8 s @ −20 dBFS: momentary max −19.9986 and short-term max −20.0000 against
    `analyze_samples(...).integrated_lufs` = −20.0000 (deltas 0.0014 LU / 0.0000 LU, tolerance 0.05);
    `loudness_range_lu` = 0.000020 LU (tolerance < 0.1).
  - Two 12 s halves 10 dB apart → **LRA = 10.0000 LU** (tolerance 10 ± 0.5), with momentary ≥ short-term on both
    this file and the stationary tone.
  - AIFF/WAV parity: identical numbers through `read_audio` for the same audio (`-19.9989 / -19.9988`).
  - Runtime: whole `op_loudness_tests` binary 0.34 s native; a 24 s stereo file measures in well under a second
    (the 24-tap interpolator runs over ~70M MACs).
  - Demo artifact: **not applicable** — no render path, engine DSP or output of existing commands changed; the 9
    golden WAVs are bit-identical (`run_goldens.sh`, evidence above). The JSON samples above are the observable
    new output.

- **PROPOSE / BLOCKED:**
  - `PROPOSE:` `opd analyze` still reads with `read_wav` while `opd loudness` uses `read_audio`, so `analyze`
    rejects AIFF input that `loudness` accepts. Aligning them is a one-line additive change but touches WP-0144's
    contract — needs its own package (and a spec line in `docs/spec/analyze.md`).
  - `PROPOSE:` add an `opd loudness` block to `harness/test_cli_errors.sh` (the eight exit-code checks above are
    currently run by hand; this package's Files section did not authorize editing harness scripts, so they were
    not added here). Suggested as a small follow-up harness WP.
  - No BLOCKED items; no test disabled, no golden/fixture file touched.
