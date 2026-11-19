---
id: WP-0154
title: Step sequencer backend — set_step / clear_steps operations, step_grid view, `opd steps`
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.5
human_review: none
---

## Context — why this exists
The prototype goal is "arrange a 3-minute beat in the app". Beats are made on a channel rack: a row of
16 step buttons per instrument. The GUI is not here yet; this package builds the **engine side only**,
as a *view over existing pattern notes* (no new model fields): two new operations the GUI/copilot will
call, a function that renders a pattern as `x...x...x...x...`, and a CLI to print it.

## Spec — what "correct" means
**`docs/spec/operations.md` (updated in this branch, section "Step sequencer — additive (WP-0154)") is
normative — read it fully. Do not edit it.** Summary:
- Step `s` = beat `s * 0.25` (16 per 4/4 bar). A note is *on step s* iff its `t == s*0.25` exactly; a
  step is on iff any note is on it. Notes not on the grid are never touched.
- `set_step {pattern, step, on, velocity?, pitch?}` and `clear_steps {pattern}`: exact semantics,
  validation order and error strings are in the spec (idempotent; `on:false` removes all notes on the
  step; `on:true` on an already-on step only edits the lowest-index note's `vel`/`note`, never
  `t`/`dur`; new notes are `{"t": s*0.25, "dur": 0.25}` appended at the end).
- Engine: `std::string op::step_grid(const Pattern& pattern, int steps);` declared in
  `src/engine/ops.hpp`, implemented in `src/engine/ops.cpp` (no new source files, no CMake change).
- CLI: `opd steps <proj.opj> <pattern-index> [--steps N]` in `src/cli/main.cpp` (add it to `usage()`
  and to the command dispatch), output exactly as in the spec.

Implementation hints (not optional style): add the two ops as new `else if` branches in the existing
`apply_named` using the existing helpers (`check_keys`, `arg_index`, `has_bool`, `has_integer`,
`fits_int`...); edit the `json` working copy and let the existing save→parse validation run. Compare
`t` as doubles with `==` against `step * 0.25` (that is intended); read `t` as `nj.value("t", 0.0)`.
Add doc comments with a usage example on `step_grid` in the style of `ops.hpp`.

## Acceptance criteria
- [ ] Unit tests in `tests/test_main.cpp` (same CHECK style; add a `test_step_sequencer()` and call it
      from `main`), at least 12 new CHECKs covering: set_step on creates the default note (no `note`/`vel`
      keys) / with `velocity`+`pitch`; set_step twice → identical `save_project` bytes; `on:false`
      removes all notes on the step but leaves an off-grid note (`t = 0.3`) and another step's note;
      `on:true` on an existing long note keeps its `dur`; `clear_steps` keeps off-grid notes;
      each rejection class in the spec (`'step' out of range` for 4096 and -1, `'step' must be an integer`
      for `1.0`, `'pitch' out of range`, `note.vel must be in (0, 1]`, `unknown argument`, on:false with
      velocity) leaves the project unchanged; `History` undo/redo restores bytes exactly; `step_grid`
      output for 16 and 8 steps, `steps = 0` → `""`.
- [ ] `opd steps` per spec; add cases to `harness/test_cli_errors.sh` in its existing style for: bad
      pattern index (exit 2), `--steps 0` (exit 2), missing file (exit 3), pattern index out of range
      (exit 2), and one success (exit 0, output `x...x...x...x...` for a project built with
      `opd apply` of four `set_step` ops).
- [ ] Determinism: a pattern built by four `set_step` ops at steps 0,4,8,12 on a sine track renders
      **byte-identically** to a hand-written project whose pattern has notes `t = 0,1,2,3`, `dur = 0.25`
      (add this as a unit test using `render_project` or as a harness script — your choice).
- [ ] All gates green with zero warnings (`-Werror` default ON): configure+build
      (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4`), `./build/op_tests`,
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`;
      sanitizer build (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`) `op_tests` passes.

## Out of scope — do NOT touch
- Any GUI/JUCE code (`src/app/`, `src/doc/`), model fields / `.opj` format, `src/engine/model*`,
  `src/engine/render*`, golden fixtures (`tests/fixtures/`), `ai-sidecar/`, `docs/adr/`, `spikes/`,
  `docs/spec/` (the updated operations spec is already in the branch), **ROADMAP.md**.
- Pattern lengths other than 4/4 16ths, swing, per-step probability/ratchets, per-step pitch lanes,
  multi-pitch chord editing (a `PROPOSE:` note is fine).

## Git
- Branch `wp-0154-step-sequencer-ops` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0154: step sequencer ops (set_step, clear_steps, step_grid, opd steps)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff
  filled and `docs/spec/operations.md` as provided.
- **Do not push or merge.** Build with `-j4` at most.

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/ops.cpp`: `set_step` and `clear_steps` branches in `apply_named` (validation order and
    error strings per spec; edits applied on the JSON working copy, then the existing save→parse path).
    Helpers `int_in_range`, `on_grid`, `kStepLimit`. `step_grid()` implementation.
  - `src/engine/ops.hpp`: declaration + doc comment with usage example for `op::step_grid`.
  - `src/cli/main.cpp`: `opd steps <proj.opj> <pattern-index> [--steps N]` (`cmd_steps`, `parse_decimal`),
    dispatch entry, usage line.
  - `tests/test_main.cpp`: `test_step_sequencer()` (19 CHECKs, called from `main`).
  - `harness/test_cli_errors.sh`: 6 `steps_*` cases.
  - `docs/spec/operations.md`: untouched (came pre-installed in the branch, committed unchanged).
  - No model fields, no `.opj` format change, no CMake change, no new source files, no goldens touched.
- **How verified:** (commands + actual output)
  - Build (Release, `-Werror` default): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2`
    → `[100%] Built target op_tests`, zero warnings (`grep -c warning` = 0). (`-j2` per the shared-machine rule.)
  - `./build/op_tests` → 539 PASS lines, 19 of them `step.*`, last line `ALL PASS (0 failures)`. Step lines:
    ```
    PASS step.set_on_default_note
    PASS step.set_on_velocity_pitch
    PASS step.set_twice_identical_bytes
    PASS step.off_removes_all_on_step_keeps_offgrid
    PASS step.on_existing_keeps_dur
    PASS step.clear_keeps_offgrid
    PASS step.reject_step_4096
    PASS step.reject_step_minus1
    PASS step.reject_step_float
    PASS step.reject_pitch_128
    PASS step.reject_vel_zero
    PASS step.reject_vel_above_one
    PASS step.reject_unknown_argument
    PASS step.reject_velocity_on_false
    PASS step.grid_16
    PASS step.grid_8
    PASS step.grid_0_empty
    PASS step.history_undo_redo_bytes
    PASS step.render_byte_identical_to_handwritten
    ```
  - `bash harness/test_cli_errors.sh` → `PASS steps_bad_index (exit 2)`, `PASS steps_steps_zero (exit 2)`,
    `PASS steps_missing_file (exit 3)`, `PASS steps_index_range (exit 2)`, `PASS steps_ok (exit 0, x...x...x...x...)`,
    `test_cli_errors: 0 failures` (exit 0).
  - `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures` (exit 0).
  - `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh` →
    `test_glue_scripts: 0 failures` (exit 0).
  - Sanitizers: `cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san -j2 --target op_tests`
    → exit 0, 0 warnings; `./build-san/op_tests` → `ALL PASS (0 failures)`, 0 `runtime error` / ASAN lines.
  - Manual `opd steps` checks (stderr / exit code):
    ```
    steps <p>            -> error: usage: steps needs <proj.opj> <pattern-index>        exit 2
    steps <p> x          -> error: usage: bad pattern index 'x'                         exit 2
    steps <p> -1         -> error: usage: bad pattern index '-1'                        exit 2
    steps <p> 0 --steps  -> error: usage: unknown arg '--steps'                         exit 2
    steps <p> 0 --steps 300 -> error: usage: bad --steps value '300'                    exit 2
    steps <p> 0 extra    -> error: usage: unknown arg 'extra'                           exit 2
    steps <p> 1          -> error: usage: pattern index 1 out of range (project has 1 patterns)  exit 2
    steps none.opj 0     -> error: not_found: none.opj                                  exit 3
    ```
    Success on a project built by `opd apply` of four `set_step` ops at steps 0,4,8,12:
    `steps <p> 0` → `x...x...x...x...` (exit 0); `steps <p> 0 --steps 8` → `x...x...`;
    empty pattern → `................`.
  - Determinism: `step.render_byte_identical_to_handwritten` renders the four-`set_step` pattern and a
    hand-written pattern (notes t=0,1,2,3, dur 0.25) with `render_project` and compares the float buffers with `memcmp`: equal.
- **Follow-ups noticed (not done):**
  - `opd steps` accepts a pattern index of 19+ digits as `bad pattern index` (usage, exit 2) rather than
    `out of range`; exit code is the same, message differs slightly. Not worth a change.
  - The task file says `-j4`; the runner instruction capped the build at `-j2`, so `-j2` was used.
- **PROPOSE / BLOCKED:** none.
