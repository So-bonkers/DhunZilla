---
id: WP-0155
title: Transport loop region in the project model (loop, set_loop/clear_loop ops, info, render --loop)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.25
human_review: none
---

## Context — why this exists
The app will soon play the arrangement live and needs a loop region (repeat bars A–B while arranging a
beat). The region is part of the project (saved, undoable, scriptable), so it belongs in the model first.
This package adds the data, validation, operations and CLI surface only. **Offline rendering is
unchanged**: `opd render` without `--loop` always renders the whole song; the loop is for live playback
later. Projects without a loop must save byte-for-byte as before.

## Spec — normative
The spec files are provided with this task and are already in the repository
(`docs/spec/project-model.md` §"Loop region — additive (WP-0155)" and `docs/spec/operations.md`
§"Loop region operations — additive (WP-0155)"). **Do not edit them.** Read both sections fully; they fix
every error string, key name and output format. Summary of the decisions:
- JSON: `"loop": {"start_bar": 1.0, "end_bar": 3.0}`, optional, 0-based bars, **fractional allowed**,
  `0 <= start_bar < end_bar <= length_bars`.
- Canonical save: key `loop` written only when set (the JSON dump sorts keys alphabetically, as for every field).
- Ops: `set_loop {start_bar,end_bar}`, `clear_loop {}`; `opd diff` emits them (steps 3b / 4b).
- `opd info`: extra line `loop: <A>-<B>` (`%g`) only when set.
- `opd render --loop`: exactly the bytes of `--from-bar A --to-bar B`.

## Implementation (exact names)
1. `src/engine/model.hpp`: add `struct LoopRegion { double start_bar = 0.0; double end_bar = 0.0; };`
   and the member `std::optional<LoopRegion> loop;` on `Project` (include `<optional>`).
2. `src/engine/model.cpp`: parse `loop` in `project_from_json` after `tempo_map` and before
   `attachments`, with exactly the spec's error strings and order; emit it in `project_to_json`
   only when `p.loop` has a value (keys `start_bar`, `end_bar`; the dump orders them alphabetically).
3. `src/engine/ops.cpp`: `set_loop` and `clear_loop` in `apply_named`, plus the two `diff_projects`
   emission steps (3b `clear_loop`, 4b `set_loop`). Reuse `check_keys` / `has_number`; do not duplicate
   range checks (they come from the model parse). `clear_loop` removes the `loop` key from the JSON copy
   (`j.erase("loop")`).
4. `src/cli/main.cpp`: `info` line; `render --loop` (add `[--loop]` to the render line of `usage()`).
   Implementation hint: after the project is loaded, when `--loop` is given set `a = loop.start_bar`,
   `b = loop.end_bar` and take the same code path as `--from-bar/--to-bar` so the bytes are identical by
   construction.
5. `tests/test_main.cpp`: add at least 8 new `CHECK`s: parse/save round-trip with a fractional loop;
   canonical bytes of a no-loop project unchanged (compare with `project_bytes` of the same project parsed
   from text without `loop`); each of the seven validation errors; `set_loop`/`clear_loop` through
   `apply_op` including an undo via `History` restoring identical bytes; `set_length_bars` below
   `loop.end_bar` rejected; `diff_projects` round trip with a loop on one side.
6. `harness/test_cli_errors.sh`: append cases for `--loop` (no loop → exit 2; combined with `--from-bar`
   → exit 2; ok → exit 0 and output equals `--from-bar/--to-bar`). Do not modify existing lines.

## Acceptance criteria
- [ ] All existing goldens unchanged: `bash harness/run_goldens.sh` green, no file under
      `tests/fixtures/` modified.
- [ ] A project parsed from JSON without `loop` has `project_bytes` identical to before this change.
- [ ] Round trip: parse → save → parse keeps `loop` (including fractional values) and a second save is
      byte-identical.
- [ ] Every validation error in the spec reproduced verbatim (via `opd info` exit 4 / `opd apply` exit 5).
- [ ] `opd apply` with `set_loop` then `clear_loop` works; undo restores prior bytes.
- [ ] `opd info` shows `loop: A-B` when set and nothing when absent.
- [ ] `opd render --loop` output is byte-identical to `--from-bar A --to-bar B`; `opd render` without
      `--loop` is unchanged even when the project has a loop.
- [ ] `opd diff` between projects that differ in loop yields a journal that reproduces the target.
- [ ] Gates: build with 0 warnings (`-Werror`), `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`, and a sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` passes. Build with `-j4` max.

## Out of scope — do NOT touch
- Engine render (`src/engine/render.cpp`), MIDI import/export, `.opd` bundle code, `src/app/`,
  `src/doc/` (the read accessor is `doc.project().loop`; no Document change).
- Live playback / transport code, loop-aware rendering, count-in.
- `docs/spec/*` (already provided), `docs/adr/`, `ai-sidecar/`, existing fixtures, **ROADMAP.md** (the
  elephant adds rows at ship time).

## Git
- Branch `wp-0155-loop-region` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0155: transport loop region (model, set_loop/clear_loop, info, render --loop)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent

- **What changed:**
  - `src/engine/model.hpp`: `struct LoopRegion {start_bar, end_bar}`; `std::optional<LoopRegion> loop` on `Project`.
  - `src/engine/model.cpp`: `loop` parsed after `tempo_map`, before `attachments`, with the seven spec error strings
    in spec order; emitted only when set (keys `start_bar`/`end_bar`, canonical alphabetical order).
  - `src/engine/ops.cpp`: `set_loop {start_bar,end_bar}` and `clear_loop {}` in `apply_named` (range rules come from
    the model parse; `clear_loop` does `j.erase("loop")`); `diff_projects` step 3b (`clear_loop`) and step 4b
    (`set_loop`), both gated on `loop_differs` (absent counts as distinct).
  - `src/cli/main.cpp`: `info` prints `loop: %g-%g` after the project line (and after `master_gain_db=`), only when set;
    `render --loop` validates option syntax before load (`--loop cannot be combined with --from-bar/--to-bar`),
    then after load (`project has no loop region`) sets `bar_a/bar_b` to the loop and takes the existing
    `--from-bar/--to-bar` path. `usage()` line updated with `[--loop]`.
  - `tests/test_main.cpp`: new `test_loop_region()` (32 CHECKs: round trip, canonical key order, absent-not-saved,
    all 7 validation errors, set/clear/undo/redo byte-exact, op argument errors, set_length_bars rejection, diff round trips).
  - `harness/test_cli_errors.sh`: 11 new cases appended before the summary line; no existing line modified.
  - Not touched: `docs/spec/*` (runner-installed, committed unchanged), fixtures, ROADMAP.md, render.cpp, src/doc, src/app.
- **How verified:**
  - Build (`cmake -DCMAKE_BUILD_TYPE=Release`, `cmake --build build -j2 --target opd op_tests`): 0 warnings, `-Werror` on.
  - `./build/op_tests` (Release): 552 PASS, 0 FAIL, exit 0 (see below).
    ```
    $ ./build/op_tests ; echo exit=$?
    exit=0
    $ grep -c "^PASS" wp0155_ut.log
    552
    ```
  - Sanitizer build (`-DDZ_SANITIZE=ON`, `build-asan`, `-j2`): builds clean; `./build-asan/op_tests` →
    `552 PASS`, `ALL PASS (0 failures)`, exit 0, no ASAN/UBSAN reports.
  - `bash harness/run_goldens.sh` →
    ```
    PASS demo: deterministic + golden match   (arranged, demo, imported, mixer, panorama, pitched, resampled, tempo: all PASS)
    PASS negative-control (corrupt golden rejected)
    run_goldens: 8 fixtures, 0 failures   (exit 0)
    ```
    `git status --short tests/fixtures` is empty.
  - `bash harness/test_cli_errors.sh build/opd` → the 11 new `loop_*` lines all PASS; `test_cli_errors: 0 failures`.
  - `UV_CACHE_DIR=<workspace>/.cache/uv bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures` (exit 0).
  - Real CLI evidence (`build/opd`, scratch project = demo golden + loop):
    - No-loop project info, then after `set_loop 0.5 1.5`:
      ```
      $ opd apply base.opj set.opjl -o loop.opj   -> apply: 1 op(s) -> loop.opj   (exit 0)
      $ opd info loop.opj
      project: .../loop.opj bpm=120.00 bars=2.00 tracks=3 patterns=3 render=192000 frames (4.000s) seed=42
      loop: 0.5-1.5
        [0] Metronome
      $ grep -A3 '"loop"' loop.opj
        "loop": {
          "end_bar": 1.5,
          "start_bar": 0.5
        },
      ```
      No-loop `opd info base.opj` prints no `loop` line (exit 0).
    - `clear_loop` then `cmp base.opj cleared.opj` → identical (`CLEAR_RESTORES_BYTES: identical`).
    - Validation via `opd info` (exit 4), each verbatim:
      ```
      error: parse: loop must be an object
      error: parse: loop.start_bar and loop.end_bar must be numbers
      error: parse: loop.start_bar must be a finite number >= 0
      error: parse: loop.end_bar must be greater than loop.start_bar
      error: parse: loop.end_bar must be <= length_bars
      error: parse: loop has unknown key 'x'
      ```
    - Validation via `opd apply` (exit 5, nothing created):
      ```
      error: apply: line 1: set_loop: loop.end_bar must be greater than loop.start_bar
      error: apply: line 1: set_loop: missing argument 'end_bar'
      error: apply: line 1: set_loop: 'start_bar' must be a number
      error: apply: line 1: set_loop: loop.end_bar must be <= length_bars
      error: apply: line 1: clear_loop: unknown argument 'x'
      $ opd apply loop.opj len.opjl(set_length_bars 1.0)
      error: apply: line 1: set_length_bars: loop.end_bar must be <= length_bars     exit=5
      ```
    - `render --loop` vs `--from-bar 0.5 --to-bar 1.5`: both `rendered 96000 frames @48000 Hz`, `cmp` identical (`LOOP_BYTES_IDENTICAL_TO_FROM_TO`).
      `--loop --from-bar 0` → `error: usage: --loop cannot be combined with --from-bar/--to-bar` (exit 2).
      `--loop` on a no-loop project → `error: usage: project has no loop region` (exit 2).
      Render of the looped project without `--loop` is byte-identical to the no-loop render (full song).
    - `opd diff base.opj loop.opj` → `{"end_bar":1.5,"op":"set_loop","start_bar":0.5}`; applying it to base reproduces loop.opj
      byte-for-byte (`DIFF_ADD_ROUNDTRIP_OK`). The reverse diff emits `{"op":"clear_loop"}` and reproduces base
      (`DIFF_REMOVE_ROUNDTRIP_OK`). A second save of a loop project is byte-identical (`SECOND_SAVE_BYTE_IDENTICAL`).
- **PROPOSE / BLOCKED:**
  - No blockers. Acceptance criteria are all met with evidence above.
  - Finding: nlohmann rejects JSON numbers that overflow double (`1e999` → `json: number overflow parsing`, exit 4), so
    spec validation item 4 (`loop.end_bar must be a finite number`) is unreachable from a `.opj`/journal file. It is
    implemented as specified (defensive, NaN/inf-safe) and covered by a unit test that feeds an infinite double to
    `project_from_json` directly. Spec wording may want a note; not changed here (spec is read-only for this WP).
  - Unit tests write under `/tmp` like the existing tests, which is the repo convention; no change made.
