# `opd peaks` — min/max waveform peaks per bin (spec)

Status: normative (WP-0171). The arrangement view of the app draws sample and bounce waveforms; drawing from raw
samples every frame is slow and the GUI must not own decoding or binning logic. This module reduces an audio file
to N min/max bins per channel (deterministic, no libm), and `opd peaks` emits them as JSON so the app, the sidecar
and tests share one definition.

## Files
- `src/engine/peaks.hpp`, `src/engine/peaks.cpp` (new). No libm of any kind.
- `CMakeLists.txt`: purely ADDITIVE edits (do not change or delete any existing line):
  1. directly after the line `target_include_directories(opengine PUBLIC src third_party)` add
     `target_sources(opengine PRIVATE src/engine/peaks.cpp)  # WP-0171`;
  2. immediately before the comment line starting `# Our targets only` add:
     `add_executable(op_peaks_tests tests/test_peaks.cpp)`,
     `target_link_libraries(op_peaks_tests PRIVATE opengine)`,
     `add_test(NAME peaks COMMAND op_peaks_tests)`;
  3. directly after the line `set(DZ_OUR_TARGETS opengine dzdoc opd op_tests)` add
     `list(APPEND DZ_OUR_TARGETS op_peaks_tests)  # WP-0171`.
- `tests/test_peaks.cpp` (new; do NOT edit `tests/test_main.cpp`): own `main`, local `CHECK(cond, name)` macro
  printing `PASS name` / `FAIL name` (copy the style from `tests/test_main.cpp`), returns the failure count.
- `src/cli/main.cpp`: ONE additive block - `#include "engine/peaks.hpp"` immediately after the
  `engine/ops.hpp` include; one usage line immediately BEFORE the `opd analyze` usage line; `static int
  cmd_peaks(...)` immediately BEFORE the line `int main(int argc, char** argv) {`; one dispatch line
  `if (cmd == "peaks") return cmd_peaks(argc - 1, argv + 1);` immediately AFTER the
  `if (cmd == "analyze")` dispatch line. No existing line may be modified or removed. `cmd_peaks` may use the
  existing `parse_decimal` and `fail` helpers.
- Do NOT edit `src/engine/wav.*` or any other engine file.

## API (exactly this, namespace `op::peaks`)
```cpp
#pragma once
#include <string>
#include <vector>
namespace op::peaks {
constexpr int kMaxBins = 65536;
struct Peaks {
    int sample_rate = 0;
    int channels = 0;                     // 1 or 2
    long long frames = 0;                 // samples per channel
    int bins = 0;
    std::vector<std::vector<float>> min;  // [channel][bin]
    std::vector<std::vector<float>> max;  // [channel][bin]
};
Peaks compute_peaks(const std::vector<float>& interleaved, int channels, int sample_rate, int bins);
std::string to_json(const Peaks& p);
}
```

## Definitions
- Invalid input (channels not in {1,2}, `bins < 1` or `bins > kMaxBins`): return a `Peaks` with
  `sample_rate`/`channels` set, `bins == 0`, `frames == 0` and empty `min`/`max`.
- Otherwise `frames = interleaved.size() / channels`, `min`/`max` have `channels` rows of `bins` values.
  `frames == 0` -> every value is `0.0f`.
- Bin `b` (0-based) covers frames `[start, end)` with `start = b*frames/bins` and `end = (b+1)*frames/bins`
  (integer division on `long long`). If `end == start` the bin covers the single frame `start`
  (`end = start + 1`; `start < frames` always holds when `frames >= 1`). So with fewer frames than bins
  neighbouring bins repeat the same frame.
- `min[c][b]` / `max[c][b]` = smallest / largest sample of channel c over the bin's frames. Non-finite samples
  (NaN, +-inf, or magnitude beyond 1e30) count as `0.0f`.
- `to_json`: one compact line, no spaces, terminated by `\n`:
  `{"version":1,"sample_rate":R,"channels":C,"frames":F,"bins":N,"min":[[v,...],...],"max":[[v,...],...]}`
  every value printed with `%.6f`; the text `-0.000000` is printed `0.000000`. Invalid-input `Peaks`
  prints `"bins":0,"min":[],"max":[]`.

## CLI
`opd peaks <in.wav> [--bins N]` - reads the file with `op::wav::read_audio` (WAV or AIFF), prints
`to_json(compute_peaks(...))` to stdout and nothing else. `--bins` default 1024. `N` must be a decimal integer
in `[1, 65536]` (use `parse_decimal` with max 65536 and reject 0). Exit codes via `fail(...)`: no file, second
positional, unknown option, valueless `--bins` or bad N -> 2; file missing -> 3; unreadable audio -> 4 (message =
the reader's error). Errors print nothing on stdout.
