---
id: WP-0180
title: Song-structure ops `insert_bars` / `delete_bars` - shift clips and the loop region, undoable, diff-safe
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [spec:operations, ops, tests:main]
---

## Context - why this exists
Arranging a 3-minute beat means adding a bar before the drop or cutting a bar out of the intro. Today that is a
hand-written sequence of `move_clip`/`set_length_bars`/`set_loop`. This package adds two operations that do it
atomically and undoably. All other files are untouched.

## Spec - normative
`docs/spec/operations.md` (new section "Song-structure operations - additive (WP-0180)") is **already updated by
the runner** before you start. Read it first: it is the contract (argument checks and their order, exact error
strings, clip and loop rules). Do not edit it. Summary (the spec section wins on any difference):

| op | JSON |
|---|---|
| `insert_bars` | `{"op":"insert_bars","at_bar":3,"count":2}` |
| `delete_bars` | `{"op":"delete_bars","at_bar":3,"count":2}` |

- `at_bar`, `count` are JSON **integers** (`1.0` rejected: `'at_bar' must be an integer`). Check order: unknown
  key, `at_bar` present/integer, `count` present/integer, `at_bar` in `[0, 1000000000]`
  (`'at_bar' out of range`), `count` in `[1, 4096]` (`'count' out of range [1..4096]`), non-empty `tempo_map`
  (`tempo_map is not supported`), then `insert_bars`: `at_bar > length_bars` -> `'at_bar' out of range`;
  `delete_bars`: `at_bar + count > length_bars` -> `range exceeds length_bars`; `delete_bars` that would leave an
  existing non-empty arrangement with no clip -> `would remove every clip`. Errors are `<op>: <reason>`.
- `insert_bars`: `length_bars += count`; clips with `start_bar >= at_bar` move later by `count`; others unchanged;
  loop `[ls,le)`: `ls >= at` both `+= count`, else `le > at` -> `le += count`, else unchanged.
- `delete_bars` (range `[a, b)`, `b = a + count`): clips with `start_bar >= b` move earlier by `count`; clips with
  `a <= start_bar < b` are removed; clips with `start_bar < a` keep only the statements `i` in `[0, repeats)` with
  `start_bar + i * (pattern_extent(pattern) / beats_per_bar) < a` (`repeats` becomes that count, >= 1). Loop bounds map
  through `f(x) = x (x <= a) | a (a < x < b) | x - count (x >= b)`; if `f(le) <= f(ls)` the loop is removed.
  `length_bars -= count` (a result of 0 is rejected by the model parse: `length_bars must be > 0`).
- No new diff emission: `opd diff` output is unchanged, but a project and the same project after either op must
  diff to a journal that reproduces it byte-exactly (this already holds; a test pins it).

## Implementation guide (2 code files)
1. `src/engine/ops.cpp`:
   - Add in the first anonymous namespace (just before its closing `}  // namespace`, which precedes the comment
     `// Applies one named op;`) a helper
     `bool shift_bars(Project& q, bool insert, long long at, long long count, std::string& err)` implementing the
     spec rules on a `Project` copy (`q.arrangement`, `q.loop`, `q.length_bars`, `q.tempo_map`, `q.patterns`,
     `q.beats_per_bar`; `pattern_extent(const Pattern&)` is declared in `engine/model.hpp`). It returns false
     with `err` set to the bare reason for the project-dependent errors (tempo_map, at_bar past end, range
     exceeds, every clip).
   - Add one `else if (name == "insert_bars" || name == "delete_bars")` branch in `apply_named` (next to
     `set_loop`): `check_keys`, `has_integer` for `at_bar` then `count`, the two range checks, then
     `Project q = p; shift_bars(...); j = project_to_json(q);` so the existing save->parse validation
     runs on the result (no second ruleset). **Pitfall:** the existing `int_in_range` ignores its lower bound for
     unsigned JSON values (a parsed `0` is unsigned), so do not use it for `count` (`count: 0` must be rejected);
     test both bounds yourself.
   - Do NOT modify `diff_projects`, `History`, `model.*`, `render.*` or the CLI.
2. `tests/test_main.cpp`: new `static void test_insert_delete_bars()` called from `main` right after
   `test_pattern_swing();`, **>= 25 new CHECKs**, using `tmp_file(...)` for any file. Cover: insert middle / at 0 / at
   `length_bars` (append) / at loop start / at loop end; delete with clips removed, trimmed repeats, shifted, a
   straddling statement kept; loop clamped / removed / shifted; every error string above (incl. `count` 0 parsed
   from JSON text); legacy project without arrangement; `History` undo/redo byte-exact and rejected op leaves
   `undo_depth()` unchanged; diff round trip (apply op, `diff_projects(base, result)`, apply the journal to base,
   compare `project_bytes`).

## Acceptance criteria
- [x] `opd apply <proj> <journal> -o <out>` with `{"op":"insert_bars","at_bar":3,"count":2}` /
      `{"op":"delete_bars",...}` yields exactly the spec'd project; rejects exit 5 with the spec'd strings and write
      nothing.
- [x] Example project: 8 bars, clips `(p0,start 0,x1) (p0,2,x4) (p1,3,x2) (p2,1,x3,-3 dB) (p0,5,x1) (p0,6.5,x1)` with
      pattern extents 1, 2, 1.5 bars, loop `[1,5)`: insert (3,2) -> length 10, starts `[0,2,5,1,7,8.5]`, loop `[1,7)`;
      delete (3,2) -> length 6, `(p0,0,x1) (p0,2,x1) (p2,1,x2) (p0,3,x1) (p0,4.5,x1)`, loop `[1,3)`.
- [x] Undo/redo restores exact bytes; `opd diff` round trips (see above).
- [x] Existing goldens bit-identical; build 0 warnings (`-Werror`, `-j4`); `build/op_tests` ends `ALL PASS`;
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh` green;
      sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` passes.
- [x] ADR-0004: no clock, randomness or libm.

## Out of scope - do NOT touch
- `ROADMAP.md`, `src/app/`, `.github/`, `ai-sidecar/`, golden fixtures (`tests/fixtures/`), `docs/adr/`,
  `docs/spec/*` (already updated), `src/engine/model.*`, `src/engine/render.cpp`, `src/cli/main.cpp`,
  `src/doc/`, `CMakeLists.txt`.
- Fractional `at_bar`/`count`, tempo-map shifting, splitting clips at the cut, an `opd` subcommand, diff emission of
  these ops, UI.

## Git
- Branch `wp-0180-insert-delete-bars` from `main`. Commit: `WP-0180: insert_bars / delete_bars song-structure ops`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

- **What changed:** two code files, +487 lines, no other file touched (plus the runner-installed
  `docs/spec/operations.md`, committed unchanged as instructed).
  - [src/engine/ops.cpp](src/engine/ops.cpp): new first-anonymous-namespace helper
    `shift_bars(Project&, bool insert, long long at, long long count, std::string&)` (+ the small
    `as_long_long()` integer reader and the `kBarMax` / `kBarsPerOpMax` constants) implementing the
    spec's clip/loop/length rules; one new `else if (name == "insert_bars" || name == "delete_bars")`
    branch in `apply_named` next to `set_loop` — strict keys → `has_integer('at_bar')` →
    `has_integer('count')` → `'at_bar' ∈ [0, 1e9]` → `'count' ∈ [1, 4096]` → `shift_bars` on a copy →
    the existing save→parse validation. `int_in_range` was deliberately **not** used for `count`
    (it ignores its lower bound for unsigned JSON values, so `count: 0` would have passed); both
    bounds are read explicitly and tested. `diff_projects`, `History`, `model.*`, `render.*` and the
    CLI are untouched.
  - [tests/test_main.cpp](tests/test_main.cpp): new `test_insert_delete_bars()` (called from `main`
    right after `test_pattern_swing();`) — **66 new CHECKs**, all scratch files via `tmp_file(...)`.

- **How verified:** every command below was run on the final tree; output pasted verbatim.

  1) Clean configure + full build, `-Werror` (CMake default `DZ_WERROR=ON`), `-j4`, zero warnings:
  ```
  $ cmake -S . -B ../.scratch/build-wp0180 -DDZ_BUILD_APP=OFF && cmake --build ../.scratch/build-wp0180 -j4
  fresh build exit: 0
  warning lines: 0
  [100%] Linking CXX executable op_tests
  [100%] Built target op_tests
  ```

  2) Unit tests (`build/op_tests`): `ALL PASS`, and the 66 new checks are all present.
  ```
  $ ./build/op_tests | tail -1
  ALL PASS (0 failures)
  $ ./build/op_tests | grep -c '^PASS sb\.'
  66
  ```
  Coverage of those 66: insert middle / at 0 / at `length_bars` / at loop start / at loop end;
  delete with clips removed + repeats trimmed + later clips shifted + a straddling statement kept
  untrimmed; trimming by a **derived** `pattern_extent` (no `length_beats`) and an empty pattern
  keeping all repeats; loop grown / moved / clamped to the cut / swallowed → removed / unchanged;
  every error string in the spec including `count: 0` parsed from JSON text on both ops, plus three
  check-order pins (`unknown argument` before missing args, `'count' out of range [1..4096]` before
  the past-the-end check, `tempo_map is not supported` before it); legacy project without an
  `arrangement` (key stays absent after either op) and `delete_bars: length_bars must be > 0`;
  `History` undo/redo byte-exact + rejected op leaves `undo_depth()` and the bytes unchanged; diff
  round trips for both ops.

  3) Golden renders — bit-identical, determinism re-checked twice per fixture:
  ```
  $ bash harness/run_goldens.sh
  PASS arranged: deterministic + golden match
  ... (12 fixtures, one line each) ...
  PASS negative-control (corrupt golden rejected)
  run_goldens: 12 fixtures, 0 failures          exit 0
  ```

  4) CLI exit-code contract: `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`, exit 0.

  5) Glue-script e2e (`UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`):
  ```
  $ bash harness/test_glue_scripts.sh
  PASS rel_no_repo_strays
  test_glue_scripts: 0 failures                 exit 0
  ```

  6) Sanitizers (`-DDZ_SANITIZE=ON`, ASAN+UBSAN), clean build and run, no diagnostics:
  ```
  $ cmake -S . -B ../.scratch/build-san-wp0180 -DDZ_BUILD_APP=OFF -DDZ_SANITIZE=ON \
      && cmake --build ../.scratch/build-san-wp0180 -j4 --target op_tests
  san build exit: 0        (grep -c warning → 0)
  $ ../.scratch/build-san-wp0180/op_tests | tail -2
  PASS format_position.clamps_past_end
  ALL PASS (0 failures)                          exit 0, no ASAN/UBSAN report
  ```

  7) Acceptance example through `opd apply` (8 bars, clips `(p0,0,x1) (p0,2,x4) (p1,3,x2)
     (p2,1,x3,-3 dB) (p0,5,x1) (p0,6.5,x1)`, extents 1 / 2 / 1.5 bars, loop `[1,5)`):
  ```
  $ opd apply example.opj <(echo '{"op":"insert_bars","at_bar":3,"count":2}') -o example_ins.opj
  apply: 1 op(s) -> example_ins.opj                                    exit 0
  length_bars: 10.0 loop: {'end_bar': 7.0, 'start_bar': 1.0}
  starts: [0.0, 2.0, 5.0, 1.0, 7.0, 8.5]
  clips: [(0, 0.0, 1), (0, 2.0, 4), (1, 5.0, 2), (2, 1.0, 3), (0, 7.0, 1), (0, 8.5, 1)]
  gains: [None, None, None, -3.0, None, None]

  $ opd apply example.opj <(echo '{"op":"delete_bars","at_bar":3,"count":2}') -o example_del.opj
  apply: 1 op(s) -> example_del.opj                                    exit 0
  length_bars: 6.0 loop: {'end_bar': 3.0, 'start_bar': 1.0}
  clips: [(0, 0.0, 1), (0, 2.0, 1), (2, 1.0, 2), (0, 3.0, 1), (0, 4.5, 1)]
  gains: [None, None, -3.0, None, None]
  ```
  Exactly the spec'd results (lengths 10 / 6, starts `[0,2,5,1,7,8.5]` and the five surviving
  clips, loops `[1,7)` / `[1,3)`).

  8) Rejections exit 5 with the spec'd strings and write nothing (`wrote=no` means the `-o` output
  file was not created; `line 1:` is `opd apply`'s journal-line prefix):
  ```
  unknown key                    exit=5 wrote=no | line 1: insert_bars: unknown argument 'oops'
  missing at_bar                 exit=5 wrote=no | line 1: insert_bars: missing argument 'at_bar'
  at_bar float 1.0               exit=5 wrote=no | line 1: insert_bars: 'at_bar' must be an integer
  missing count                  exit=5 wrote=no | line 1: insert_bars: missing argument 'count'
  count float 2.5                exit=5 wrote=no | line 1: delete_bars: 'count' must be an integer
  at_bar negative                exit=5 wrote=no | line 1: insert_bars: 'at_bar' out of range
  at_bar > 1e9                   exit=5 wrote=no | line 1: insert_bars: 'at_bar' out of range
  count 0 (JSON int)             exit=5 wrote=no | line 1: delete_bars: 'count' out of range [1..4096]
  count 4097                     exit=5 wrote=no | line 1: insert_bars: 'count' out of range [1..4096]
  count negative                 exit=5 wrote=no | line 1: insert_bars: 'count' out of range [1..4096]
  insert past end                exit=5 wrote=no | line 1: insert_bars: 'at_bar' out of range
  delete range exceeds           exit=5 wrote=no | line 1: delete_bars: range exceeds length_bars
  remove every clip              exit=5 wrote=no | line 1: delete_bars: would remove every clip
  tempo_map insert               exit=5 wrote=no | line 1: insert_bars: tempo_map is not supported
  tempo_map delete               exit=5 wrote=no | line 1: delete_bars: tempo_map is not supported
  tempo_map before past-end      exit=5 wrote=no | line 1: insert_bars: tempo_map is not supported
  legacy delete to zero          exit=5 wrote=no | line 1: delete_bars: length_bars must be > 0
  count-range before past-end    exit=5 wrote=no | line 1: insert_bars: 'count' out of range [1..4096]
  ```
  Legacy (no `arrangement`) applies succeed and keep the key absent:
  `length 7.0 has_arrangement False` after `insert_bars(2,3)`, `length 1.0 has_arrangement False`
  after `delete_bars(1,3)`.

  9) Diff round trip at CLI level — no new op emitted, journal reproduces the result byte-exactly:
  ```
  $ opd diff example.opj example_ins.opj -o ins_journal.opjl && cat ins_journal.opjl
  {"op":"clear_loop"}
  {"length_bars":10.0,"op":"set_length_bars"}
  {"end_bar":7.0,"op":"set_loop","start_bar":1.0}
  {"index":2,"op":"move_clip","start_bar":5.0}
  {"index":4,"op":"move_clip","start_bar":7.0}
  {"index":5,"op":"move_clip","start_bar":8.5}
  $ opd apply replay.opj ins_journal.opjl -o replay.opj && cmp -s replay.opj example_ins.opj
  REPLAY IDENTICAL (insert)
  # delete_bars: 13-op journal (remove_clip / clear_loop / set_length_bars / set_loop /
  #   set_clip_repeats / set_clip_pattern / move_clip / set_clip_gain …) → REPLAY IDENTICAL (delete)
  ```

  10) ADR-0004 — added lines contain no clock, randomness or libm calls (only integer/double
  comparisons and arithmetic; `pattern_extent` is the existing model function):
  ```
  $ git diff -U0 -- src/engine/ops.cpp | grep '^+' | \
      grep -Ei "chrono|::time|gettimeofday|rand\(|random|srand|clock|sqrt|pow\(|sin\(|cos\(|exp\(|log\(|fabs|std::hypot"
  (no matches)
  ```

- **Metrics:** `src/engine/ops.cpp` +110, `tests/test_main.cpp` +377; 66 new CHECKs; existing test
  count unchanged and green; 12 goldens bit-identical; build time ~2 min (`-j4`); no new
  dependencies, no new diff emission, no ADR needed (no architecture decision).

- **PROPOSE / BLOCKED:**
  - PROPOSE: the runner-installed `docs/spec/operations.md` replaced the whole
    "Pattern duplicate operation — additive (WP-0179)" section with this package's section and also
    dropped the WP-0178/WP-0179 sentences from §`opd diff`, while `duplicate_pattern`,
    `set_clip_fades` and the `patterns[i].id` unsupported-difference rule are still implemented in
    `src/engine/ops.cpp`. I committed the file exactly as installed (per instructions, no rewording),
    but the spec now under-documents two shipped features — someone should re-add those sections.
  - PROPOSE: `int_in_range()`'s unsigned lower-bound blind spot is a trap for the next package; it
    is out of WP-0180 scope to change, so `as_long_long()` + explicit bounds were used here instead.
  - BLOCKED: none.
