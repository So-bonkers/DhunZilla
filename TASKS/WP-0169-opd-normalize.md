---
id: WP-0169
title: `opd normalize` - gain a WAV to a target integrated LUFS under a sample-peak ceiling (new engine module)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context - why this exists
The end of "arrange a 3-minute beat" is a bounced file that is neither too quiet nor clipping. `opd analyze`
(WP-0144) measures integrated LUFS and peak, and `opd render` writes the mix, but nothing applies the gain that
the measurement implies. This package adds a NEW engine module `op::normalize` (plan the gain, apply it) and an
`opd normalize` command that writes the result. It reuses `op::analyze::analyze_samples` unchanged.

## Spec - normative
Create the NEW file `docs/spec/normalize.md` containing the content of this whole "Spec" section (headings may
be adapted). Do not edit other spec files.

### Files
- `src/engine/normalize.hpp`, `src/engine/normalize.cpp` (new).
- `CMakeLists.txt`: purely ADDITIVE edits (do not change or delete any existing line):
  1. directly after the line `target_link_libraries(dzdoc PUBLIC opengine)` add
     `target_sources(opengine PRIVATE src/engine/normalize.cpp)  # WP-0169`;
  2. immediately before the comment line starting `# Our targets only` add:
     `add_executable(op_normalize_tests tests/test_normalize.cpp)`,
     `target_link_libraries(op_normalize_tests PRIVATE opengine)`,
     `add_test(NAME normalize COMMAND op_normalize_tests)`;
  3. directly after the line `set(DZ_OUR_TARGETS opengine dzdoc opd op_tests)` add
     `list(APPEND DZ_OUR_TARGETS op_normalize_tests)  # WP-0169`.
- `tests/test_normalize.cpp` (new; do NOT edit `tests/test_main.cpp`): its own `main`, a local
  `CHECK(cond, name)` macro printing `PASS name` / `FAIL name` (copy the style from `tests/test_main.cpp`),
  returns the failure count.
- `src/cli/main.cpp`: ONE additive block - `#include "engine/normalize.hpp"` after the `engine/model.hpp`
  include; one usage line inserted immediately BEFORE the `opd steps` usage line; `static int cmd_normalize(...)`
  placed immediately BEFORE the comment of `parse_decimal` (the line starting `// Parse a decimal argument`);
  one dispatch line `if (cmd == "normalize") return cmd_normalize(argc - 1, argv + 1);` immediately BEFORE the
  `if (cmd == "steps")` line. No existing line may be modified or removed. Reuse the existing helpers
  `parse_bar_value` (strict finite-number parse) and `fail`.
- Do NOT edit `src/engine/analyze.*`, `src/engine/dsp.hpp`, `src/engine/wav.*` or any other engine file. libm
  only via `op::dsp::` wrappers (`powd_` etc.); no direct `std::pow/log10/...` in `normalize.cpp`.

### API (exactly this, namespace `op::normalize`)
```cpp
#pragma once
#include <string>
#include <vector>
namespace op::normalize {
struct GainPlan {
    double input_lufs = 0.0;         // integrated loudness of the input (op::analyze)
    double input_peak_dbfs = 0.0;    // sample peak of the input (op::analyze peak_dbfs)
    double requested_gain_db = 0.0;  // target_lufs - input_lufs
    double gain_db = 0.0;            // the gain to apply
    bool limited = false;            // true iff the ceiling reduced the gain
};
bool plan_gain(const std::vector<float>& interleaved, int channels, int sample_rate, double target_lufs,
               double ceiling_dbfs, GainPlan& plan, std::string& err);
void apply_gain_db(std::vector<float>& samples, double gain_db);
}
```
`plan_gain` (checks in this order; each failure returns false with exactly this `err`):
1. `target_lufs` not in `[-70, 0]` (NaN fails): `target LUFS out of range [-70, 0]`;
2. `ceiling_dbfs` not in `[-30, 0]` (NaN fails): `ceiling out of range [-30, 0]`;
3. run `op::analyze::analyze_samples(interleaved, channels, sample_rate)`; if its `integrated_lufs` or
   `peak_dbfs` is empty: `no measurable loudness (silent or too short)`.
Then `input_lufs`, `input_peak_dbfs` from the analysis; `requested_gain_db = target_lufs - input_lufs`;
`allowed = ceiling_dbfs - input_peak_dbfs`; `limited = requested_gain_db > allowed`;
`gain_db = limited ? allowed : requested_gain_db`. (`gain_db` may be negative: a peak above the ceiling is
attenuated even when the loudness is already below target.)
`apply_gain_db`: `g = static_cast<float>(op::dsp::powd_(10.0, gain_db / 20.0))`; every sample `s[i] = s[i] * g`.
Gain 0 dB therefore leaves every sample bit-identical.

### CLI
`opd normalize <in.wav> -o <out.wav> --lufs <T> [--ceiling-db <C>] [--format pcm16|pcm24|f32]`
- Options may appear in any order. `--ceiling-db` default `-1.0`; `--format` default `pcm16`.
- Number arguments must parse whole and finite (`parse_bar_value`). Usage errors (exit 2, nothing written):
  missing `<in.wav>`, missing `-o`, missing `--lufs`, unknown/extra argument, option without value, bad number,
  unknown format (same message as `opd render`), `--lufs` outside `[-70, 0]`, `--ceiling-db` outside `[-30, 0]`.
  All usage checks happen BEFORE the input file is touched.
- Input missing -> exit 3 (`not_found`). Unreadable audio (`op::wav::read_audio`; WAV or AIFF) -> exit 4
  (`parse`). Sample rate outside [8000, 192000] -> exit 5, code string `normalize`,
  `unsupported sample rate <N>`. `plan_gain` failure -> exit 5, code string `normalize`, message = its `err`.
  Output write failure -> exit 5 (`write`). On any error the output file is not created.
- A mono input is first duplicated into stereo (L = R); loudness is measured on that stereo buffer (so the
  written file measures at the target) and the output is always stereo. The output is written with
  `op::wav::write_wav_stereo` at the input sample rate.
- On success stdout is exactly one line:
  `{"input_lufs":%.4f,"input_peak_dbfs":%.4f,"gain_db":%.4f,"limited":true|false}` + `\n`.

## Acceptance criteria
- [ ] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`).
- [ ] `tests/test_normalize.cpp` with at least 8 CHECKs: unlimited plan reaches the target (apply, re-analyze
      with `analyze_samples`: within 0.01 LU); limited plan (loud input, target 0 LUFS, ceiling -1) has
      `limited == true`, `gain_db < requested_gain_db`, and the re-analyzed sample peak is -1.0 +/- 0.01 dBFS;
      each of the four error strings exactly; `apply_gain_db(v, 0.0)` is the identity; `apply_gain_db(v, 20.0)`
      multiplies by ~10.
- [ ] `opd normalize` works as specified (exit codes, stdout line, mono -> stereo).
- [ ] Standard gates: build (0 warnings, `-j4`), `op_tests`, `op_normalize_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of
      `op_tests` and `op_normalize_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `src/app/`, `.github/`, golden fixtures (`tests/fixtures/`),
  `docs/spec/` other than the new `normalize.md`, `docs/adr/`, `ai-sidecar/`, `tests/test_main.cpp`,
  `src/engine/analyze.*`, `src/engine/dsp.hpp`, `src/engine/wav.*`, `src/engine/model.*`, `src/engine/ops.cpp`,
  `src/doc/`.
- True-peak-aware ceilings, limiters/compressors, batch mode, dither, resampling.

## Git
- Branch `wp-0169-opd-normalize` from `main`. Commit: `WP-0169: opd normalize - target LUFS gain with peak ceiling`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:** Branch `wp-0169-opd-normalize` from `main`. All new files, plus purely additive edits
  (verified: `git diff | grep '^-[^-]'` → 0 removed lines).
  - NEW `docs/spec/normalize.md` — the Spec section above, verbatim (headings adapted to spec-file style).
  - NEW `src/engine/normalize.hpp` / `src/engine/normalize.cpp` — `op::normalize::{GainPlan, plan_gain, apply_gain_db}`
    exactly per API; reuses `op::analyze::analyze_samples` unchanged; libm only via `op::dsp::powd_`; range checks
    rely on NaN failing every comparison (no `<cmath>` calls in normalize.cpp).
  - `CMakeLists.txt` — the three specified additive insertions (`target_sources`, test target + `add_test(NAME normalize)`,
    `list(APPEND DZ_OUR_TARGETS op_normalize_tests)`).
  - NEW `tests/test_normalize.cpp` — own `main`, CHECK macro copied from `test_main.cpp` style, returns failure count; 14 CHECKs.
  - `src/cli/main.cpp` — the four additive insertions only (include after `engine/model.hpp`; usage line before `opd steps`;
    `cmd_normalize` before the `// Parse a decimal argument` comment; dispatch line before `if (cmd == "steps")`).
    Reuses `parse_bar_value` and `fail`. No existing line modified or removed.

- **How verified:** (all commands run on this aarch64 rig; outputs pasted)
  1. **Fresh build, `-j4`, zero warnings** (`cmake -S . -B …build-fresh && cmake --build … -j4 | grep -ciE "warning|error"`):
     `0` (grep exit=1, no matches).
  2. **`op_normalize_tests`: 14/14 PASS** (spec's required CHECKs: unlimited reaches target ±0.01 LU; limited plan
     flags + peak at −1.0 ±0.01 dBFS after apply; all error strings exact — incl. NaN targets/ceilings, silent and
     too-short inputs; 0 dB identity is `memcmp` bit-identical incl. −0.0/denormal/fmax values; +20 dB × ~10):
     ```
     PASS plan.unlimited_fields
     PASS plan.unlimited_reaches_target
     PASS plan.limited_fields
     PASS plan.limited_peak_at_ceiling
     PASS plan.negative_gain_attenuates
     PASS err.target_above_range
     PASS err.target_below_range
     PASS err.target_nan
     PASS err.ceiling_below_range
     PASS err.ceiling_nan
     PASS err.silent
     PASS err.too_short
     PASS apply.zero_bit_identical
     PASS apply.twenty_db_times_ten
     ALL PASS (0 failures)
     ```
  3. **Goldens bit-identical** (`bash harness/run_goldens.sh`): `run_goldens: 9 fixtures, 0 failures` + `PASS negative-control`.
  4. **`op_tests`**: `ALL PASS (0 failures)`; ctest registration works: `ctest -R normalize` → `100% tests passed, 0 tests failed out of 1`.
  5. **`harness/test_cli_errors.sh`**: `test_cli_errors: 0 failures`. **`harness/test_glue_scripts.sh`** (with
     `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`): `test_glue_scripts: 0 failures`.
  6. **Sanitizers** (`build-san`, `DZ_SANITIZE:BOOL=ON`): `op_tests` and `op_normalize_tests` rebuilt under
     ASAN+UBSAN and run: both `ALL PASS (0 failures)`, no sanitizer reports.
  7. **CLI contract evidence** (fixtures: copy of `tests/fixtures/goldens/demo.wav`, mono/silent/4 kHz WAVs made with
     Python stdlib in a temp dir; input measures −13.1453 LUFS / peak −1.3665 dBFS):
     ```
     $ opd normalize in.wav -o out.wav --lufs -16          # exit 0, exactly 1 stdout line (wc -l = 1)
     {"input_lufs":-13.1453,"input_peak_dbfs":-1.3665,"gain_db":-2.8547,"limited":false}
     $ opd analyze out.wav | grep integrated               # re-measurement lands on target
       "integrated_lufs": -16.0000,

     $ opd info mono.wav                                   # mono input …
     wav: …/mono.wav rate=48000 ch=1 frames=192000 duration=4.000s
     $ opd normalize mono.wav --lufs -20 --ceiling-db -3 -o out_mono.wav   # exit 0 (options any order)
     {"input_lufs":-11.8641,"input_peak_dbfs":-1.3665,"gain_db":-8.1359,"limited":false}
     $ opd info out_mono.wav                               # … stereo output, measured on the L=R buffer
     wav: …/out_mono.wav rate=48000 ch=2 frames=192000 duration=4.000s
       "integrated_lufs": -20.0000,
     $ opd normalize --format f32 -o out_f32.wav --lufs -18 in.wav         # arbitrary order + f32: exit 0
     {"input_lufs":-13.1453,"input_peak_dbfs":-1.3665,"gain_db":-4.8547,"limited":false}
     ```
     Usage errors — every case exit 2 and the output file was NOT created (`PASS …-no-output` for all):
     missing `<in.wav>`, missing `-o`, missing `--lufs`, unknown arg (`--wat`), extra positional, option without value
     (trailing `--lufs`), bad number (`abc`, `inf`), `--lufs 5`, `--lufs -71`, `--ceiling-db -40`, `--ceiling-db 1`.
     Unknown format message is identical to `opd render`:
     ```
     error: usage: unknown format 'mp3' (expected pcm16, pcm24 or f32)   # render and normalize byte-identical
     ```
     Runtime errors — exit code + stderr verbatim, output never created:
     ```
     missing input        -> exit 3: error: not_found: …/nope.wav
     unreadable audio     -> exit 4: error: parse: <reader error>
     bad sample rate      -> exit 5: error: normalize: unsupported sample rate 4000
     silent input         -> exit 5: error: normalize: no measurable loudness (silent or too short)
     unwritable output    -> exit 5: error: write: cannot open for write '…/nodir/out.wav'
     ```
  - **Metrics:** 77 added lines in the two existing files (0 removed); 14 unit CHECKs; no change to any existing
    audio path — all 9 goldens bit-identical, so demo before/after WAVs are not applicable (only a new command's
    output is new; its evidence is the re-measured LUFS lines above).

- **PROPOSE / BLOCKED:** none. Sidecar pytest + ruff not run: no Python file was touched (WP out of scope for `ai-sidecar/`).
