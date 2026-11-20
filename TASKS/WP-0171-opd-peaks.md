---
id: WP-0171
title: `opd peaks` - min/max waveform peaks per bin for sample and bounce waveform drawing (new engine module)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context - why this exists
The arrangement view of the app has to draw sample and bounce waveforms. Drawing from raw samples every frame
is slow and the GUI should not own decoding or binning logic. This package adds a NEW engine module
`op::peaks` that reduces an audio file to N min/max bins per channel (deterministic, no libm), plus an
`opd peaks` command emitting them as JSON so the app, the sidecar and tests share one definition.

## Spec - normative
Create the NEW file `docs/spec/peaks.md` containing the content of this whole "Spec" section (headings may be
adapted). Do not edit other spec files.

### Files
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

### API (exactly this, namespace `op::peaks`)
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
### Definitions
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

### CLI
`opd peaks <in.wav> [--bins N]` - reads the file with `op::wav::read_audio` (WAV or AIFF), prints
`to_json(compute_peaks(...))` to stdout and nothing else. `--bins` default 1024. `N` must be a decimal integer
in `[1, 65536]` (use `parse_decimal` with max 65536 and reject 0). Exit codes via `fail(...)`: no file, second
positional, unknown option, valueless `--bins` or bad N -> 2; file missing -> 3; unreadable audio -> 4 (message =
the reader's error). Errors print nothing on stdout.

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`).
- [x] `tests/test_peaks.cpp` with at least 10 CHECKs: a hand-computed mono 8-frame/4-bin case (each bin's
      min and max), a stereo 3-frame/1-bin case, fewer frames than bins (2 frames, 5 bins: bin layout
      0,0,0,1,1 of frame index), empty input -> zeros with the requested bin count, invalid arguments ->
      `bins == 0`, NaN/inf treated as 0, exact JSON text for a tiny case, no `-0.000000` in the output.
- [x] `opd peaks` works as specified.
- [x] Standard gates: build (0 warnings, `-j4`), `op_tests`, `op_peaks_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of
      `op_tests` and `op_peaks_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `src/app/`, `.github/`, golden fixtures (`tests/fixtures/`),
  `docs/spec/` other than the new `peaks.md`, `docs/adr/`, `ai-sidecar/`, `tests/test_main.cpp`,
  `src/engine/wav.*`, `src/engine/model.*`, `src/engine/ops.cpp`, `src/doc/`.
- Multi-resolution pyramids, caching on disk, RMS bins, GUI drawing code.

## Git
- Branch `wp-0171-opd-peaks` from `main`. Commit: `WP-0171: opd peaks - min/max waveform peaks per bin`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - NEW `docs/spec/peaks.md` — full normative Spec section copied from this work package.
  - NEW `src/engine/peaks.hpp` / `src/engine/peaks.cpp` — `op::peaks` module exactly per the spec API block
    (plus house-style doc comments). No libm: only comparisons, integer arithmetic and stdio formatting;
    non-finite/huge samples (`!(x >= -1e30 && x <= 1e30)`, false for NaN/±inf/>1e30) count as `0.0f`;
    bins initialized from each bin's first sample so all-negative/all-positive bins are exact.
  - `CMakeLists.txt` — purely ADDITIVE (diff: 6 insertions, 0 deletions): the three blocks exactly where specified.
  - NEW `tests/test_peaks.cpp` — own `main`, local CHECK macro copied from test_main.cpp style; 24 CHECKs.
  - `src/cli/main.cpp` — purely ADDITIVE (diff: 37 insertions, 0 deletions): include after `engine/ops.hpp`,
    usage line before the `opd analyze` line, `cmd_peaks` immediately before `main`, dispatch line immediately
    after the `analyze` dispatch. Uses existing `parse_decimal` (max 65536, rejects 0) and `fail`.
- **How verified:** (all commands run on branch `wp-0171-opd-peaks`, aarch64 DGX Spark, g++ 13)
  - [x] Existing goldens bit-identical:
    ```
    $ bash harness/run_goldens.sh
    PASS mixer: deterministic + golden match
    PASS panorama: deterministic + golden match
    PASS pitched: deterministic + golden match
    PASS resampled: deterministic + golden match
    PASS sampler_trim: deterministic + golden match
    PASS tempo: deterministic + golden match
    PASS negative-control (corrupt golden rejected)
    run_goldens: 9 fixtures, 0 failures
    ```
  - [x] `tests/test_peaks.cpp` — all required cases covered (hand-computed mono 8f/4b per-bin min+max, stereo
        3f/1b, 2f/5b layout 0,0,0,1,1, empty→zeros at requested bin count, invalid args→`bins==0`,
        NaN/±inf/>1e30→0, exact JSON text (valid + invalid-input forms), no `-0.000000`). 24 CHECKs:
    ```
    $ ./build/op_peaks_tests
    PASS mono8x4.shape ... PASS json.no_negative_zero_exact   (24 lines)
    op_peaks_tests: 0 failure(s)          # exit 0
    $ cd build && ctest -R peaks
    1/1 Test #2: peaks ............................   Passed    0.00 sec
    ```
  - [x] `opd peaks` works as specified (WAV + AIFF, determinism, defaults, exit-code contract, empty stdout on errors):
    ```
    $ ./build/opd peaks tests/fixtures/goldens/kits/kick.wav --bins 6 | head -c 400
    {"version":1,"sample_rate":48000,"channels":2,"frames":14400,"bins":6,"min":[[-0.742432,-0.511353,...
    $ ./build/opd peaks ... --bins 6 > p1.json && ./build/opd peaks ... --bins 6 > p2.json && cmp p1 p2   # bit-exact
    $ ./build/opd peaks kits/kick.wav | grep -o '"bins":1024'      # default bins
    "bins":1024
    # AIFF path (minimal mono AIFF 8f@8000Hz generated with stdlib python; values hand-checked vs /32768):
    {"version":1,"sample_rate":8000,"channels":1,"frames":8,"bins":4,"min":[[0.000000,0.250000,-1.000000,...
    # exit codes (stdout empty on every error):
    no file -> 2 | second positional -> 2 | unknown option -> 2 | valueless --bins -> 2
    bad bins 0/abc/65537/-1 -> 2 | missing file -> 3 | unreadable audio -> 4 (stderr: "error: parse: file too small...")
    ```
  - [x] Standard gates:
    ```
    $ cmake --build build -j4                    # exit 0, zero warnings (-Werror on our targets)
    $ ./build/op_tests | tail -2                 # ALL PASS (0 failures)
    $ bash harness/test_cli_errors.sh            # test_cli_errors: 0 failures
    $ UV_CACHE_DIR=.../qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh   # test_glue_scripts: 0 failures
    $ cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4 --target op_tests op_peaks_tests
    $ ./build-san/op_peaks_tests                 # 0 failure(s)   (ASAN+UBSAN clean)
    $ ./build-san/op_tests                       # ALL PASS (0 failures)   (ASAN+UBSAN clean)
    ```
- **Metrics:** engine module peaks.cpp = 90 lines, no libm (`#include <cstdio>`, `<cstring>` only); diff vs main:
  CMakeLists +6/-0, cli/main.cpp +37/-0; new files: peaks.hpp, peaks.cpp, test_peaks.cpp, docs/spec/peaks.md.
- **PROPOSE / BLOCKED:** none.

