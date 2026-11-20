---
id: WP-0162
title: Pattern length_beats - explicit loop period so a 16-step pattern with empty tail steps loops on the bar
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context - why this exists
A pattern's loop period ("extent") is derived as `max(note.t + note.dur)`. A 16-step drum pattern built with
`set_step` whose last steps are empty therefore loops EARLY and drifts out of the bar. This package adds an
optional explicit `length_beats` to a pattern, uses it as the extent, adds two operations, and teaches
`opd steps` and `opd diff` about it. The normative text is the new section "Explicit pattern length (WP-0162)"
at the end of `docs/spec/project-model.md` and "Pattern length operations (WP-0162)" at the end of
`docs/spec/operations.md` (both already updated in the repo; read them, do not edit them).

## Spec - exact interface
```cpp
// src/engine/model.hpp
struct Pattern { int track = 0; std::vector<Note> notes; std::optional<double> length_beats; };
double pattern_extent(const Pattern& pat);   // namespace op; defined in src/engine/model.cpp
```
- `pattern_extent`: `*length_beats` if present, else `max(t_beats + dur_beats)` over notes (0.0 if none).
- Parse (`load_project`): key `length_beats` on a pattern object. Present but not a number, `<= 0`, or not
  finite -> error exactly `pattern.length_beats must be a finite number > 0`. Check it after the notes.
- Save (`project_to_json`): write `"length_beats"` (double) only when present. Absent -> byte-identical to now.
- Renderer (`src/engine/render.cpp`, arrangement path) and MIDI export (`src/engine/midi.cpp`): replace the
  inline extent loop by `pattern_extent(pat)`. Keep `reps = (extent > 0.0) ? clip.repeats : 1`. Notes are NOT
  truncated, wrapped or dropped: a note at `t = 2.5` in a `length_beats = 2` pattern still plays at
  `base + r*2 + 2.5`. No-arrangement path untouched.
- Ops (`src/engine/ops.cpp`):
  - `{"op":"set_pattern_length","pattern":<int>,"length_beats":<number>}` : keys check, `pattern` via the
    existing integer-index accessor, `length_beats` must be a number (`missing argument 'length_beats'` /
    `'length_beats' must be a number`), then write `patterns[pattern].length_beats`; the positivity rule comes
    from the model parse (`set_pattern_length: pattern.length_beats must be a finite number > 0`).
  - `{"op":"clear_pattern_length","pattern":<int>}` : erase the key; success even if absent.
- Diff (`diff_projects`): new step 7b exactly as in the operations spec (emit `set_pattern_length` /
  `clear_pattern_length` after the note edits, before `add_pattern`). No new "unsupported" case.
- `opd steps` (`src/cli/main.cpp`): when `--steps` is not given and the pattern has `length_beats`, width =
  `ceil(length_beats*4)` clamped to `[1,256]`. Explicit `--steps` wins. Otherwise 16.
  Update the `opd` usage text only if it mentions the steps default.

## Acceptance criteria
- [x] Everything above implemented; all existing goldens bit-identical (`bash harness/run_goldens.sh`) — evidence in Handoff report.
- [x] New CHECKs in `tests/test_main.cpp` (49 added, ≥8), using a per-test unique temp dir
      (`std::filesystem::temp_directory_path()` + name containing the pid) if files are needed: pattern with
      notes at beats 0,1,2,3 (dur 0.25) + `length_beats=4` repeated 3 times -> kicks at beats 0..11 (sample =
      beat*24000 at 120 bpm / 48 kHz) and WITHOUT the field at 0,1,2,3,3.25,4.25,...; a note past the length is
      kept; validation errors (0, -1, string); save omits the key when absent and round-trips when
      present; ops incl. History undo/redo byte-exact; diff round trip (both directions); `step_grid`
      width via `opd steps` default.
- [x] Standard gates all green (see Handoff report): build (0 warnings, `-j4`), `op_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `src/app/`, golden fixtures (`tests/fixtures/goldens/`), `docs/spec/*`
  (already updated), `ai-sidecar/`, `docs/adr/`.
- No `length_beats` clipping of notes, no UI, no new CLI command.

## Git
- Branch `wp-0162-pattern-length` from `main`. Commit: `WP-0162: pattern length_beats - explicit loop period (render, ops, steps, diff)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - `src/engine/model.hpp`: `Pattern::length_beats` (`std::optional<double>`); declared
    `double pattern_extent(const Pattern&)`; updated the extent doc comments.
  - `src/engine/model.cpp`: parse `length_beats` on a pattern object (checked after the notes; error
    `pattern.length_beats must be a finite number > 0` for non-number / `<= 0` / non-finite); canonical save
    emits `"length_beats"` (double, sorts before `"notes"`) only when present; defined `pattern_extent()`
    (`*length_beats` else derived `max(t+dur)`, 0.0 when empty).
  - `src/engine/render.cpp` + `src/engine/midi.cpp`: arrangement-path inline extent loops replaced by
    `pattern_extent(pat)`; `reps = (extent > 0.0) ? clip.repeats : 1` kept verbatim; no-arrangement paths
    untouched. Notes are never truncated/wrapped/dropped (placement math unchanged).
  - `src/engine/ops.cpp`: new ops `set_pattern_length` / `clear_pattern_length` with the spec's strict
    argument order (keys → `pattern` integer index → `length_beats` number; positivity from the model
    parse); clear is a no-op success when absent. `diff_projects` step 7b emits `set_pattern_length` /
    `clear_pattern_length` after note edits, before `add_pattern`; patterns added by step 8 carry their own
    field in the pattern JSON. No new "unsupported" case.
  - `src/cli/main.cpp`: `opd steps` default width = `ceil(length_beats*4)` clamped `[1,256]` when the pattern
    has the field and `--steps` was not given; explicit `--steps` wins; otherwise still 16. Usage text not
    changed (it does not mention a steps default — "16th-step grid" is the step resolution).
  - `tests/test_main.cpp`: new `test_pattern_length()` — 49 CHECKs, per-test unique temp dir
    (`temp_directory_path()/op_wp0162_<pid>`).
  - `CMakeLists.txt`: one added compile definition `OPD_BIN="$<TARGET_FILE:opd>"` on `op_tests` so the unit
    tests can drive the real `opd steps` end-to-end (same binary as each build tree, incl. sanitizers).
- **How verified:** (all commands run in this repo; full outputs below are the tail lines)
  - Build, `-j4`, zero warnings (`-Wall -Wextra -Werror`; fresh configure into `out/wp0162-clean` grepped:
    0 warning/error lines): `cmake --build out/wp0162-clean -j4` → `BUILD_EXIT=0`.
  - `./build/op_tests` → `ALL PASS (0 failures)` — 736 CHECKs total, incl. 49 new `plen.*`:
    onsets at beats 0..11 with `length_beats:4` × 3 repeats and derived-extent drift to 3.25/4.25 without it
    (sample windows around beat*24000 @120 bpm/48 kHz); note t=2.5 in a `length_beats:2` pattern plays at
    2.5 and 4.5; validation errors verbatim for `0`, `-1`, `"4"` and injected infinity, notes checked first;
    save omits the key when absent, emits `"length_beats": 4.0` before `"notes"`, round-trip byte-stable;
    ops incl. History undo/redo byte-exact and failed-op non-mutation; diff add/remove/change round trips +
    identical→no-ops; `opd steps` widths 16/32/2/clamp-256/explicit-wins/default-16 via the real binary;
    MIDI export onsets match the renderer (beats 0..11).
  - `bash harness/run_goldens.sh` → `run_goldens: 9 fixtures, 0 failures` (all bit-exact run-to-run and vs.
    committed goldens — projects without the field save/render byte-identically).
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  - `UV_CACHE_DIR=… bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - Sanitizers: `cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4` (clean) then
    `./build-san/op_tests` → `ALL PASS (0 failures)` (ASAN+UBSAN, no reports).
- **Metrics:** +49 unit CHECKs; engine diff ≈ +35 lines across 6 files + tests/CMake. No golden fixture touched.

  Real gate output (final code, this machine — aarch64 DGX Spark):

  ```text
  $ cmake -S . -B out/wp0162-clean && cmake --build out/wp0162-clean -j4   # fresh full build
  BUILD_EXIT=0; grep -Ec "warning|error" → 0                                # zero warnings with -Wall -Wextra -Werror

  $ ./build/op_tests          # 736 PASS lines total, incl. 49 new plen.* e.g.:
  PASS plen.length_loops_on_the_bar_beats_0_to_11
  PASS plen.derived_loops_early_drifts
  PASS plen.note_past_length_kept
  PASS plen.err_zero
  PASS plen.err_negative
  PASS plen.err_string
  PASS plen.err_not_finite
  PASS plen.save_omits_absent
  PASS plen.save_emits_double
  PASS plen.undo_set_restores_bytes
  PASS plen.redo_set
  PASS plen.op_set_zero_rejected_by_parse
  PASS plen.diff_add
  PASS plen.diff_remove
  PASS plen.midi_onsets_match_renderer_beats_0_to_11
  PASS plen.steps_width_clamped_256
  ALL PASS (0 failures)

  $ bash harness/run_goldens.sh
  PASS tempo: deterministic + golden match
  PASS negative-control (corrupt golden rejected)
  run_goldens: 9 fixtures, 0 failures

  $ bash harness/test_cli_errors.sh
  test_cli_errors: 0 failures

  $ UV_CACHE_DIR=<workspace>/.cache/uv bash harness/test_glue_scripts.sh
  test_glue_scripts: 0 failures

  $ cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4   # clean, no warnings
  $ ./build-san/op_tests
  ALL PASS (0 failures)                                                        # ASAN+UBSAN, no reports
  ```
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE: `opd info` could print `length_beats` per pattern someday
  (out of scope here, no spec text); left untouched.

