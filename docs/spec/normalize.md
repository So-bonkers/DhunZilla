# `opd normalize` — gain a WAV to a target integrated LUFS under a sample-peak ceiling (spec)

Status: normative (WP-0169). The end of "arrange a 3-minute beat" is a bounced file that is neither too
quiet nor clipping. `opd analyze` measures; this command applies the gain the measurement implies, capped
so the output sample peak never exceeds a ceiling. It reuses `op::analyze::analyze_samples` unchanged.

## Files
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

## API (exactly this, namespace `op::normalize`)
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

## CLI
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
