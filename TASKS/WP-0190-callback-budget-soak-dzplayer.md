---
id: WP-0190
title: `dz_latency` - Phase 0 latency/xrun report (per-block render time vs the 2.67 ms budget) over the JUCE-free dzplayer core
phase: 0
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [CMakeLists.txt, new:tools/dz_latency.cpp, new:tools/dz_latency.md, new:tools/test_dz_latency.py]
---

## Context - why this exists
Phase 0 asks for a latency/xrun number: does the playback core finish each audio callback inside its budget?
At 48 kHz and 128-sample blocks the budget is 128/48000 s = 2.6667 ms. The JUCE-free `dz::Player`
(`src/app/player.hpp`, library target `dzplayer`) already runs headless, so a small dev tool can drive
`Player::process` through simulated device callbacks, time each one, and print a report. The same run also
proves determinism: the audio the callbacks produce must equal what `opd render` writes, byte for byte.
It is a REPORT, not a CI gate: overruns never change the exit code. Wall-clock timing lives ONLY in this tool,
never in engine/model/player code (ADR-0004). No new dependencies.

## Spec - normative

### Files
- `tools/dz_latency.cpp` (new): the whole tool, with its own `main`. May include `engine/model.hpp`,
  `engine/render.hpp`, `engine/wav.hpp`, `player.hpp` and `nlohmann/json.hpp` (all already available through the
  `opengine` and `dzplayer` targets).
- `tools/dz_latency.md` (new): usage note; it must contain these lines verbatim (anything else is free):
  `# dz_latency`, `usage: dz_latency PROJECT.opj [--block N] [--budget-ms F] [--reference REF.wav]` and
  `Exit codes: 0 ok, 1 mismatch, 2 usage, 3 input, 5 render`.
- `tools/test_dz_latency.py` (new): stdlib `unittest`, at least 5 test methods, see Tests.
- `CMakeLists.txt`: purely ADDITIVE (no existing line changed or deleted):
  1. immediately before the comment line starting `# Our targets only` add
     `add_executable(dz_latency tools/dz_latency.cpp)` and
     `target_link_libraries(dz_latency PRIVATE opengine dzplayer)` (plus an optional comment line);
  2. immediately after the LAST existing line that starts with `list(APPEND DZ_OUR_TARGETS` add
     `list(APPEND DZ_OUR_TARGETS dz_latency)  # WP-0190` (so `-Werror` and the sanitizer flags reach it).
  The binary is `<build>/dz_latency` (no `add_test`; it is not a gate).
- Do NOT edit anything under `src/` (not `src/app/`, not `src/engine/`, not `src/cli/`): `std::chrono` must not
  appear in any file under `src/`.

### CLI
`dz_latency PROJECT.opj [--block N] [--budget-ms F] [--reference REF.wav]` (options in any order; exactly one
positional PROJECT; if an option is repeated the LAST occurrence wins, e.g. `--block 64 --block 128` uses 128).
- `--block N`: simulated callback size in frames, default `128`. `N` must consist only of the decimal digits
  `0-9` (so `+8`, `1e3`, `8.0`, `-1`, `abc`, `128abc` and the empty string are all errors) and be in `[1, 16384]`.
- `--budget-ms F`: override the budget. `F` must be fully consumed by `std::strtod` (so `128abc` is an error;
  `1e3` is accepted), be finite (`nan` and `inf` are errors) and be `> 0`. Default: `block * 1000.0 / sample_rate`
  (`2.666667` for 128 frames at 48000 Hz).
- `--reference REF.wav`: an `opd render PROJECT -o REF.wav` output to byte-compare against.
- The standard load project for the Phase 0 number is `tests/fixtures/goldens/arranged.opj` (4 bars, 3 tracks,
  480000 frames at 48 kHz, 3750 blocks of 128).

### Behaviour (in this order)
1. Parse arguments; any problem -> nothing on stdout, stderr `error: <reason>` then a `usage: ...` line, exit `2`.
   Problems: no PROJECT, a second positional, unknown option, missing option value, bad `--block`, bad `--budget-ms`.
2. PROJECT missing -> stderr `error: project not found: <path>`, exit `3`. `op::load_project` fails -> stderr
   `error: <loader message>`, exit `3`.
3. Render offline exactly like `opd render` without options: `op::render_project(p, <directory of PROJECT>, buf, err)`.
   Failure or zero frames -> stderr `error: render failed: <reason>`, exit `5`.
4. Step 3 must hold the render as `auto rendered = std::make_unique<op::wav::StereoBuffer>();` and call
   `op::render_project(p, dir, *rendered, err)`. Copy `rendered->interleaved` into a `std::vector<float> expected`
   and remember the sample rate, then `dz::Player player; player.submit(std::move(rendered)); player.play();`
   (`submit` takes a `std::unique_ptr<op::wav::StereoBuffer>`). Call `player.process(left, right, N)` (two
   `float` arrays of `N` elements) once per block with `N = --block`, for `blocks = ceil(total_frames / N)`
   blocks, where `total_frames` is the rendered frame count. Time ONLY the `process` call with
   `std::chrono::steady_clock` (two `now()` calls around it). Appending `left[i]`, `right[i]` to the produced
   interleaved `std::vector<float>` happens outside the timed region, and only the first `total_frames` frames
   are kept (the tail of the last block is dropped). Each block's time is in milliseconds as a `double`.
5. `output_identical` = the produced interleaved floats equal `expected` exactly (`==`).
6. `--reference`: if the file cannot be opened -> stderr `error: cannot read reference: <path>`, exit `3`
   (nothing on stdout). Otherwise write the produced audio with `op::wav::write_pcm16_stereo` (sample rate =
   the project's rendered rate) to a temporary file in `std::filesystem::temp_directory_path()` (unique name with the pid, removed on every path, including a write failure) and compare its
   bytes to the reference file's bytes: `reference_match` = equal.
7. Statistics over the per-block times: `max_ms` = largest; `p50_ms` / `p99_ms` = nearest-rank percentile on the
   ascending-sorted times, `sorted[ceil(q * n) - 1]` with `q` = 0.50 / 0.99 and `n = blocks`;
   `overruns` = number of blocks whose time is STRICTLY greater than `budget_ms`.
8. Print the JSON below to stdout. Exit `0` if `output_identical` is true and `reference_match` is true or null,
   else exit `1` (the report is still printed). Overruns NEVER affect the exit code.

### JSON (byte-exact layout: 2-space indent, keys in this order, ends with `}\n`)
```
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
`project` is the PROJECT argument as given, as a standard JSON string (escape it with
`nlohmann::json(project).dump()`). `budget_ms`, `max_ms`, `p99_ms`, `p50_ms` are printed with `%.6f`.
`reference_match` is the bare word `null` when `--reference` was not given, else `true` / `false`. Integers are
plain decimal. The timing values vary run to run; everything else is deterministic.

### Tests (`tools/test_dz_latency.py`)
Skipped (`unittest.skipUnless`) unless env vars `DZ_LATENCY_BIN`, `DZ_OPD_BIN` and `DZ_PROJECT` are all set.
Use a unique temp dir per test run (include the pid). Cover: key order and `blocks == ceil(total_frames/128)`;
`reference_match` true against a fresh `opd render` output and exit 0; `null` without `--reference`;
`--budget-ms 1000000` -> `overruns == 0` and `--budget-ms 0.000000001` -> `overruns > 0`; a one-byte-corrupted
reference -> exit 1 with `reference_match` false; exit codes 2 (no args, `--block 0`) and 3 (missing project).

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`).
- [x] `dz_latency tests/fixtures/goldens/arranged.opj --reference <opd render output>` prints the JSON above with
      `output_identical` and `reference_match` true, `blocks` 3750, `budget_ms` 2.666667, exit 0.
- [x] `--block 7` gives `blocks` 68572 (ceil); `--block 64` gives `budget_ms` 1.333333.
- [x] `--block 64 --block 128` gives `block_frames` 128.
- [x] `tools/dz_latency.md` contains the three required lines.
- [x] `--block 8.0`, `--block +8`, `--block 128abc`, `--budget-ms inf`, `--budget-ms nan` exit 2; `--budget-ms 1e3` gives
      `budget_ms` 1000.000000.
- [x] Error exit codes 2 / 3 / 5 / 1 as specified; nothing on stdout for exit 2 and 3.
- [x] Standard gates: build (0 warnings, `-j4`), `op_tests`, `harness/run_goldens.sh`, `harness/test_cli_errors.sh`,
      `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` and `dz_latency`,
      and the Python tests run with the three env vars set.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `src/` (everything), `.github/`, golden fixtures (`tests/fixtures/`),
  `docs/`, `ai-sidecar/`, `tests/test_main.cpp`, `tools/render_bench.py`.
- A real audio device, JUCE, a CI gate on overruns, multi-pass/soak loops, xrun injection, percentile
  interpolation, JSON output files, any new dependency.

## Git
- Branch `wp-0190-callback-budget-soak-dzplayer` from `main`. Commit: `WP-0190: dz_latency - per-block render time vs budget report for dzplayer`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - `tools/dz_latency.cpp` (new): the whole tool. Renders offline (`op::render_project`, project dir as base_dir),
    holds the render in a `std::make_unique<op::wav::StereoBuffer>()`, hands it to `dz::Player` via
    `submit(std::move(...))`, `play()`, then drives `process(left, right, N)` for `ceil(total_frames/N)` simulated
    callbacks. Only the two `steady_clock::now()` calls around `process` are timed; copy-out and the tail-drop to
    `total_frames` happen outside the timed region. Nearest-rank percentiles (`sorted[ceil(q*n)-1]`), strict-greater
    overruns, byte-exact JSON (2-space indent, fixed key order, `%.6f` for the four ms fields, `reference_match`
    `null` without `--reference`). Exit 0/1 only from `output_identical` + `reference_match`; overruns never affect it.
    `--block`: digits-only in `[1,16384]`; `--budget-ms`: fully-consumed `strtod`, finite, `> 0`; repeated options:
    last wins; reference check via `write_pcm16_stereo` to a pid-unique file in `temp_directory_path()`, removed on
    every path.
  - `tools/dz_latency.md` (new): usage note with the three required verbatim lines (`# dz_latency` line 1,
    `usage: ...` line 8, `Exit codes: ...` line 10).
  - `tools/test_dz_latency.py` (new): stdlib unittest, 11 methods, `skipUnless(DZ_LATENCY_BIN && DZ_OPD_BIN && DZ_PROJECT)`,
    pid-unique temp dir per run. Covers all required cases: key order + `blocks == ceil(total_frames/128)`; reference
    match true / exit 0; `null` without `--reference`; huge/tiny budget overruns 0/>0; one-byte-corrupted reference ->
    exit 1 with report printed; repeated-option last-wins; `--block 7` -> 68572 and `--block 64` -> 1.333333;
    usage errors (incl. `8.0`, `+8`, `128abc`, `inf`, `nan`) exit 2 with empty stdout; missing project exit 3.
  - `CMakeLists.txt`: purely additive, `git diff --stat` = `5 +++++`, 0 deletions (see evidence below).
  - No changes under `src/`, `docs/`, fixtures or anything else out of scope.
- **How verified:** (all commands run in the repo root; branch `wp-0190-callback-budget-soak-dzplayer`)
  1. **Build, 0 warnings, -j4** — `cmake -S . -B build && cmake --build build -j4` → exit 0; verbose compile line for the
     new tool confirms the gates reach it: `/usr/bin/c++ -I…/src -I…/third_party -I…/src/app -std=gnu++20 -Wall -Wextra
     -ffp-contract=off -Werror … -c tools/dz_latency.cpp` (no warning/error lines in the full build log; `grep -icE "warning|error"` = 0).
  2. **Phase 0 number** — `build/opd render tests/fixtures/goldens/arranged.opj -o ref.wav && build/dz_latency tests/fixtures/goldens/arranged.opj --reference ref.wav` → exit 0:
     ```
     {
       "project": "tests/fixtures/goldens/arranged.opj",
       "sample_rate": 48000,
       "block_frames": 128,
       "blocks": 3750,
       "total_frames": 480000,
       "budget_ms": 2.666667,
       "max_ms": 0.000608,
       "p99_ms": 0.000416,
       "p50_ms": 0.000320,
       "overruns": 0,
       "output_identical": true,
       "reference_match": true
     }
     ```
     **Phase 0 answer: p50 ≈ 0.0003 ms, p99 ≈ 0.0004 ms, max ≈ 0.0006 ms vs the 2.6667 ms budget — 0 overruns**
     (worst block ≈ 0.02 % of budget; headless timing is faster than a real device callback will be, but by a huge margin).
  3. **Block-size criteria** — `--block 7` → `"blocks": 68572`; `--block 64` → `"budget_ms": 1.333333`;
     `--block 64 --block 128` → `"block_frames": 128`. All exit 0.
  4. **Option validation** — `--block 8.0`, `--block +8`, `--block 128abc`, `--budget-ms inf`, `--budget-ms nan`:
     each `exit=2 stdout_len=0 stderr="error: bad --… value '…'"`; `--budget-ms 1e3` → `"budget_ms": 1000.000000,`.
  5. **Exit-code contracts** — no args → exit 2 (`error: missing PROJECT argument` + usage line, empty stdout);
     missing project → exit 3 `error: project not found: …` (empty stdout); unloadable `.opj` → exit 3 with loader message;
     sampler pointing at a missing wav → exit 5 `error: render failed: sampler: cannot open '…/kits/missing.wav'`;
     one-byte-flipped reference → exit 1, report still printed with `"reference_match": false`; unopenable reference →
     exit 3 `error: cannot read reference: …` (empty stdout).
  6. **Standard gates** — `./build/op_tests` → `ALL PASS (0 failures)`; `bash harness/run_goldens.sh` →
     `run_goldens: 13 fixtures, 0 failures`; `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`;
     `UV_CACHE_DIR=… bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  7. **Sanitizers** — `cmake -S . -B build-san-wp0190 -DDZ_SANITIZE=ON && cmake --build build-san-wp0190 -j4 --target op_tests dz_latency`
     (exit 0, no warnings); `./build-san-wp0190/op_tests` → `ALL PASS (0 failures)`; `build-san-wp0190/dz_latency … --reference …`
     → exit 0 with `output_identical`/`reference_match` true and **empty stderr** (ASAN+UBSAN clean, incl. leak checks on the Player mailbox).
  8. **Python tests** — `DZ_LATENCY_BIN=build/dz_latency DZ_OPD_BIN=build/opd DZ_PROJECT=tests/fixtures/goldens/arranged.opj python3 -m unittest -v tools/test_dz_latency.py`
     → `Ran 11 tests in 0.657s … OK`.
  9. **Scope** — `grep -rn "std::chrono" src/ | wc -l` = 0 (untouched); CMakeLists diff is additions only.
- **Metrics:** Phase 0 (arranged.opj, block 128, default build): p50 0.000320 ms, p99 0.000416 ms, max 0.000608 ms,
  overruns 0 / 3750 blocks (budget 2.666667 ms). Under ASAN: p50 0.000928 ms, p99 0.000976 ms, max 0.003456 ms, still 0 overruns.
- **PROPOSE / BLOCKED:**
  - PROPOSE: a follow-up could record the report to a file (`--json-out`) and add a soft nightly threshold; both are
    explicitly out of scope here.
  - Note for the human (no action needed): this branch already existed from an earlier WP-0190 attempt whose tip
    (`c99eb46`, based on old main `ffd2430`) was reachable **only** from it. Before resetting the branch to current
    `main` as this task specifies, that tip was preserved at `backup/wp-0190-prior-c99eb46`. The implementation here
    was written fresh against this task file, not cherry-picked from the old attempt.
