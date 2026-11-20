---
id: WP-0189
title: `opd render --normalize-lufs <L> [--ceiling-dbfs <C>]` - normalize the mix while rendering (CLI only; reuses op::normalize)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [spec:normalize, cli:main, new:harness/test_render_normalize.sh]
---

## Context - why this exists
`opd normalize` (WP-0169) gains a finished WAV to a target LUFS under a peak ceiling, but the beat-maker flow is
"render, then normalize" as two commands with an intermediate file (and an extra 16-bit quantisation). This
package lets `opd render` do it in one step: after rendering (and after any bar-range slice) the existing
`op::normalize` functions are applied to the stereo mix before anything is written. Nothing in the normalize
module changes; this is a CLI-only change plus a spec section and a small harness script.

## Spec - normative
Append a new section to the END of the existing `docs/spec/normalize.md` (purely additive: do not change or
delete any existing line of that file). Its heading line is exactly
`## `opd render --normalize-lufs` - normalize while rendering (additive, WP-0189)` with the backticks around
`opd render --normalize-lufs` only, and its body is the text below (the contract; implement exactly it).

Status: normative (WP-0189). `opd render` can apply the `op::normalize` module (`plan_gain` + `apply_gain_db`,
unchanged) to the mixed output before it is written, so a bounce lands at a target loudness in one step.
Without `--normalize-lufs` nothing changes: `opd render` output (files and stdout) stays bit-identical.

### Files
- `src/cli/main.cpp` only (`cmd_render`). No engine file, no `normalize.*`, no `CMakeLists.txt` change.
- `harness/test_render_normalize.sh` (new): CLI contract script, style of `harness/test_cli_errors.sh`.

### CLI
`opd render <proj.opj> -o <out.wav> [existing options] [--normalize-lufs <L> [--ceiling-dbfs <C>]]`
- `--normalize-lufs <L>`: target integrated LUFS. `--ceiling-dbfs <C>`: sample-peak ceiling, default `-1.0`,
  only valid together with `--normalize-lufs`. Both take a value; either may appear anywhere among the other
  options; if given twice the last one wins (as for the other render options). An option given as the last
  argument without a value falls into the existing `unknown arg '<option>'` usage error.
- Number arguments must parse whole and finite (`parse_bar_value`). Usage errors exit 2, nothing is written
  (no output, no stems dir), and are detected BEFORE the project file is loaded. After the existing option loop,
  the existing `missing -o <out.wav>` check and the existing `--loop cannot be combined ...` check, these are
  checked in exactly this order (the first failure is reported); each message follows `error: usage: `:
  1. `--normalize-lufs` not parseable: `bad --normalize-lufs value '<L as given>'`
  2. `--normalize-lufs` outside `[-70, 0]`: `--normalize-lufs must be in [-70, 0]`
  3. `--ceiling-dbfs` not parseable: `bad --ceiling-dbfs value '<C as given>'`
  4. `--ceiling-dbfs` outside `[-30, 0]`: `--ceiling-dbfs must be in [-30, 0]`
  5. `--ceiling-dbfs` given without `--normalize-lufs`: `--ceiling-dbfs requires --normalize-lufs`
  (`-70`, `0`, `-30` are accepted; `nan`, `inf`, `1e999` are "bad value".) All later existing checks
  (`--from-bar` parsing and bounds, project load) are unchanged and come after these.
- Order of operations: render the project (mix, or mix + stems when `--stems-dir` is given), slice to the bar
  range if `--from-bar/--to-bar/--loop` were used, THEN plan the gain on the sliced stereo mix:
  `op::normalize::plan_gain(mix.interleaved, 2, mix.sample_rate, L, C, plan, err)`, then
  `op::normalize::apply_gain_db(mix.interleaved, plan.gain_db)`. With `--stems-dir`, the SAME `plan.gain_db` is
  applied to every sliced stem (the stems keep summing to the mix; stem loudness is not measured separately).
  `--format` is applied when writing, after the gain (the gain happens in float, quantisation last).
- `plan_gain` failure (silent or too short mix): exit 5 with `error: normalize: <err>` (code string
  `normalize`, message = `err`, e.g. `error: normalize: no measurable loudness (silent or too short)`). In that
  case the planning has happened before any write, so NO output file, NO stems directory and no stem files are
  created. Write failures keep their existing exit 5 / `write` behaviour.
- stdout on success: the existing line(s) exactly as today (`stem ...` lines, then the `rendered ...` line),
  followed by exactly one more line, the same JSON line as `opd normalize`, printed after the `rendered` line:
  `{"input_lufs":%.4f,"input_peak_dbfs":%.4f,"gain_db":%.4f,"limited":true|false}` + `\n`, where the values
  are the `GainPlan` fields (`input_*` describe the un-normalized sliced mix). The `rendered ... frames` count
  and everything else on those lines is unchanged.
- Guarantees (same as `opd normalize`): the written file's integrated LUFS is the target within quantisation
  (pcm16: |error| < 0.05 LU for ordinary program material) unless `limited` is true, and its sample peak is
  `<= C` (plus at most one pcm LSB for integer formats). `limited` is true iff the ceiling reduced the gain.
- `opd render` usage text gains a line `[--normalize-lufs L [--ceiling-dbfs C]]` (between the existing
  render usage line and its `[--format ...]` continuation line).

### Implementation guide (all in `cmd_render` in `src/cli/main.cpp`; `#include "engine/normalize.hpp"` is already there)
1. Declare `bool has_norm = false, has_ceiling = false; std::string norm_arg, ceiling_arg; double norm_lufs = 0.0,
   norm_ceiling = -1.0;` next to the other option variables; add two option branches in the option loop, in the
   style of `--from-bar` (`else if (!std::strcmp(argv[i], "--normalize-lufs") && i + 1 < argc) { norm_arg =
   argv[++i]; has_norm = true; }`, same for `--ceiling-dbfs`).
2. Immediately BEFORE the comment `// WP-0145 validation order` add the five ordered checks of the CLI section
   (each: `usage(); return fail("usage", "<message>");` - `fail` prints `error: usage: <message>`, exit 2).
3. Plan/apply in both the plain branch and the `--stems-dir` branch, after slicing and before the first
   `write_wav_stereo` call, as described; failure -> `return fail("normalize", err);`. Print the JSON line after
   the existing `rendered ...` printf of the respective branch, only when `has_norm`.
4. Do not touch any other command, helper or existing line; no existing line is modified or removed (adding the
   usage line is an insertion).
5. `harness/test_render_normalize.sh <opd>`: executable bash script (`set -u`, copy the `check` helper style of
   `harness/test_cli_errors.sh`; `OPD="${1:-$ROOT/build/opd}"`; temp dir with trap cleanup) printing `PASS <name>` /
   `FAIL <name>` lines and ending `render-normalize: ALL PASS` (exit 0) or `render-normalize: N FAILED` (exit 1).
   At least 12 PASS checks covering: each of the five usage errors with its exact stderr text and exit 2; usage
   errors detected before the project load (non-existent project + bad value -> exit 2); nothing written on usage
   errors; happy path lands within 0.05 LU of the target (measure with `opd analyze`, `integrated_lufs`) on
   `tests/fixtures/goldens/demo.opj` with `--seed 1`; the ceiling-limited case (`--normalize-lufs -8
   --ceiling-dbfs -3` -> stdout ends `"limited":true}` and `analyze` `peak_dbfs` within 0.01 of -3); stdout is
   exactly two lines with the flag and one line without; stems written with `--stems-dir`; silent project (a
   one-sine-track project with an empty pattern, `"notes":[]`) -> exit 5, stderr contains `no measurable
   loudness`, neither output nor stems dir exists. Do NOT edit `harness/test_cli_errors.sh`.

## Acceptance criteria
- [x] Spec section appended to `docs/spec/normalize.md`; implementation per the section.
- [x] `opd render demo.opj -o out.wav --normalize-lufs -16` writes a file whose `opd analyze` integrated LUFS is
      -16.0 +/- 0.05; `--normalize-lufs -8 --ceiling-dbfs -3` gives `"limited":true` and sample peak -3.00 dBFS.
- [x] With `--format f32` the output is byte-identical to `opd render ... --format f32` followed by
      `opd normalize <that> -o <out> --lufs L [--ceiling-db C] --format f32` (same gain path), and stdout's last line is
      byte-identical to the `opd normalize` stdout line.
- [x] Without the new options everything (files, stdout, exit codes) is unchanged; goldens bit-identical.
- [x] Every error string and exit code of the CLI section exactly as written; nothing written on any error.
- [x] `harness/test_render_normalize.sh` passes (>= 12 PASS).
- [x] Standard gates: build (0 warnings, `-j4`), `op_tests`, `op_normalize_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests`
      and `opd` with `harness/test_render_normalize.sh` run against the sanitizer `opd`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `src/app/`, `.github/`, golden fixtures (`tests/fixtures/`),
  `docs/spec/` other than appending to `normalize.md`, `docs/adr/`, `ai-sidecar/`, `CMakeLists.txt`, `tests/`,
  `src/engine/` (in particular `normalize.*`, `analyze.*`, `render.*`, `wav.*`), `src/doc/`,
  `harness/test_cli_errors.sh`.
- True-peak (dBTP) ceilings, per-stem normalization, normalizing `opd click`, a `--normalize` flag on other
  commands, MP3/other formats, dithering.

## Git
- Branch `wp-0189-render-normalize` from `main`. Commit: `WP-0189: opd render --normalize-lufs / --ceiling-dbfs`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - `src/cli/main.cpp` (`cmd_render`, purely additive — no existing line modified or removed): two new options
    `--normalize-lufs <L>` / `--ceiling-dbfs <C>` (last-wins, value anywhere among options); the five ordered
    usage checks inserted immediately before the `// WP-0145 validation order` block; in both the plain and
    `--stems-dir` branches, `op::normalize::plan_gain` on the sliced stereo mix **before any write** +
    `apply_gain_db` on the mix (and the same gain on every sliced stem), placed after slicing and before the
    WP-0176 limiter/write; the `opd normalize` JSON line printed after the `rendered ...` line when the flag is
    on; usage text gained the `[--normalize-lufs L [--ceiling-dbfs C]]` line. No engine/normalize/CMake change.
  - `harness/test_render_normalize.sh` (new, executable): CLI contract script, 18 PASS checks.
  - `docs/spec/normalize.md`: spec section appended by the runner before work started; included unchanged.
- **How verified:** (all commands run on this machine, branch `wp-0189-render-normalize`)
  - Build: `cmake --build build -j4` after `touch src/cli/main.cpp` → `grep -ci warning` = **0** (`-Werror` active).
  - Acceptance — happy path + ceiling case:
    ```
    $ ./build/opd render tests/fixtures/goldens/demo.opj -o a.wav --seed 1 --normalize-lufs -16
    rendered 192000 frames @48000 Hz -> a.wav (seed=1)
    {"input_lufs":-13.1454,"input_peak_dbfs":-1.3665,"gain_db":-2.8546,"limited":false}
    $ ./build/opd analyze a.wav | grep integrated_lufs
      "integrated_lufs": -16.0000,
    $ ./build/opd render tests/fixtures/goldens/demo.opj -o b.wav --seed 1 --normalize-lufs -8 --ceiling-dbfs -3
    rendered 192000 frames @48000 Hz -> b.wav (seed=1)
    {"input_lufs":-13.1454,"input_peak_dbfs":-1.3665,"gain_db":-1.6335,"limited":true}
    $ ./build/opd analyze b.wav | grep -o '"peak_dbfs":[^,]*'
    "peak_dbfs": -3.0000
    ```
  - Acceptance — f32 equivalence and stdout line identity: covered as `f32_pipeline_byte_identical` and
    `f32_stdout_json_line_identical` (cmp) in the harness script below.
  - Acceptance — unchanged without the flags: `run_goldens.sh` **12 fixtures, 0 failures** (bit-identical),
    `test_cli_errors.sh` **0 failures**. Determinism of the new path: two renders to the same path with
    `--seed 1 --normalize-lufs -16` → WAV + stdout bit-identical (`sha256 c756821b…0c59`).
  - Acceptance — error strings/exit codes and nothing-written: harness script, full run:
    ```
    $ ./harness/test_render_normalize.sh
    PASS usage_bad_lufs (exit 2, stderr message exact)
    PASS usage_lufs_range (exit 2, stderr message exact)
    PASS usage_bad_ceiling (exit 2, stderr message exact)
    PASS usage_ceiling_range (exit 2, stderr message exact)
    PASS usage_ceiling_alone (exit 2, stderr message exact)
    PASS usage_errors_nothing_written
    PASS usage_before_load (exit 2)
    PASS happy_exit (exit 0)
    PASS happy_lufs (-16.0000 within 0.05 of -16)
    PASS stdout_two_lines_with_flag
    PASS stdout_one_line_without_flag
    PASS limited_flag_true
    PASS limited_peak (-3.0000 within 0.01 of -3)
    PASS stems_written (3 stem wavs)
    PASS silent_exit5_message (exit 5, stderr has message)
    PASS silent_nothing_written
    PASS f32_pipeline_byte_identical
    PASS f32_stdout_json_line_identical
    render-normalize: ALL PASS          (exit 0)
    ```
  - Standard gates: `op_tests` → `ALL PASS (0 failures)`; `op_normalize_tests` → `ALL PASS (0 failures)`;
    `harness/run_goldens.sh` → `run_goldens: 12 fixtures, 0 failures`; `harness/test_cli_errors.sh` →
    `test_cli_errors: 0 failures`; `UV_CACHE_DIR=… ./harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - Sanitizers: `cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4 --target op_tests opd`
    (clean, no warnings) → `./build-san/op_tests` → `ALL PASS (0 failures)`;
    `./harness/test_render_normalize.sh "$PWD/build-san/opd"` → `render-normalize: ALL PASS`
    (default ASAN options incl. LeakSanitizer — silent).
- **Metrics:** +41 lines in `src/cli/main.cpp` (additive), new harness script 95 lines / 18 PASS checks
  (>= 12 required). No engine, no normalize module, no CMake change; goldens untouched.
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE: if `--limit-dbfs` and `--normalize-lufs` are ever combined,
  the current order is normalize → limiter (limiter stays "immediately before writing" per WP-0176); the spec
  does not pin this combination — worth one clarifying sentence in a future package.

