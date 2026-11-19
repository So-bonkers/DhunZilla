---
id: WP-0159
title: `opd diff` supports clip pattern/repeats differences (uses set_clip_pattern / set_clip_repeats)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
`opd diff` (WP-0146, `op::diff_projects` in `src/engine/ops.cpp`) rejects any difference in
`arrangement[i].pattern` or `arrangement[i].repeats` of a shared clip as unsupported, because no operation
existed for them. WP-0147 added `set_clip_pattern`, `set_clip_repeats`, `duplicate_clip`, `reorder_clip`.
This package makes those two differences supported. Reorder/duplicate **detection is out of scope**: clips
are still compared position by position, so a duplicated or reordered clip is expressed by per-position field
changes plus add/remove of tail clips (correct, not minimal). `diff_projects` never emits `duplicate_clip`
or `reorder_clip`.

## Spec — what "correct" means
`docs/spec/operations.md` has ALREADY been replaced with the version containing the updated section
"`opd diff` ..." (steps 1b and 9 and the paragraph "Clip lists are compared position by position").
**Read it first and implement it exactly; it wins on any conflict.** Do not edit that file. Summary:
- `arrangement[i].pattern` and `.repeats` are removed from the unsupported list; every other unsupported
  field and its order, and every error string, stays exactly as is.
- New ops use b's canonical values: `{"index":i,"op":"set_clip_pattern","pattern":P}` and
  `{"index":i,"op":"set_clip_repeats","repeats":R}`.
- Step 1b (after all `remove_clip`, before `remove_pattern`): ONLY when `size(a.patterns) > size(b.patterns)`,
  emit `set_clip_pattern` for shared clips i ascending whose pattern differs.
- Step 9, per shared clip i ascending: `set_clip_pattern` (only if it was not done in 1b), then
  `set_clip_repeats`, then the existing `move_clip`, `set_clip_gain`.
- **Byte-identical guarantee:** any pair whose shared clips have equal `pattern` and `repeats` must produce
  exactly the journal bytes it produced before this change.
- The final self-check (apply the journal to a copy of a, compare with b) stays.

## Acceptance criteria
- [ ] `src/engine/ops.cpp` `diff_projects` implements the above (only that function's clip logic; no new
      operation, `apply_op`/`apply_journal` untouched, no signature change). Update the doc comment in
      `src/engine/ops.hpp` only if it mentions the old limitation.
- [ ] For any pair of projects a, b that differ only in supported fields (now including clip pattern,
      repeats, and any clip list, including ones produced with `duplicate_clip` / `reorder_clip`),
      `opd diff a b -o j; opd apply a j -o c` gives `c` byte-identical to `b`.
- [ ] Unit tests in `tests/test_main.cpp` (extend `test_opd_diff()`; at least 8 new CHECKs): exact journal text
      for (i) repeats on clip 0 + pattern on clip 1, (ii) all four fields changed on one clip (order pattern,
      repeats, move_clip, set_clip_gain), (iii) shrinking patterns (clip re-pointed then `remove_pattern`,
      step 1b order), (iv) growing patterns (`add_pattern` then `set_clip_pattern`); round-trip of a
      `duplicate_clip` and of a `reorder_clip` pair with no `duplicate_clip`/`reorder_clip` in the output; the
      other unsupported fields still give their exact `unsupported difference: <field>` strings. Fix any
      existing test that asserted `arrangement[i].pattern|repeats` as unsupported.
- [ ] Add one check to `harness/test_cli_errors.sh` (existing `check` style) that `opd diff` on a pair differing
      only in a clip's repeats exits 0.
- [ ] Gates: build with 0 warnings (`-Werror`), `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`; sanitizer build
      (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`) `op_tests` pass. Build with `-j4` at most.

## Out of scope — do NOT touch
- New operations, `apply_op`, reorder/duplicate/move detection, `src/doc/`, `src/app/`, `docs/spec/*`,
  `docs/adr/`, golden fixtures, `ai-sidecar/`, `ROADMAP.md` (the elephant adds the row).

## Git
- Branch `wp-0159-diff-clip-ops` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0159: opd diff supports clip pattern/repeats (set_clip_pattern, set_clip_repeats)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/ops.cpp` `diff_projects`: removed the `arrangement[i].pattern` / `.repeats` unsupported
    checks. Added step 1b (set_clip_pattern before remove_pattern, only when `size(a.patterns) > size(b.patterns)`),
    and in step 9 per shared clip: `set_clip_pattern` (unless done in 1b), `set_clip_repeats`, `move_clip`,
    `set_clip_gain`. No signature, `apply_op` or `apply_journal` change; no new op.
  - `src/engine/ops.hpp`: doc comment no longer lists clips' pattern/repeats as unsupported.
  - `tests/test_main.cpp` `test_opd_diff()`: 13 new CHECKs (groups g1..g5 plus `diff.unsupported_pattern_track_1`):
    exact journals for repeats+pattern, all four fields on one clip, shrinking patterns, growing patterns;
    replay round trips for those and for `duplicate_clip` / `reorder_clip` pairs (journal never names those ops).
  - `harness/test_cli_errors.sh`: `diff_clip_repeats_ok` (arranged.opj with clip 2 repeats 1→2, exit 0).
  - `docs/spec/operations.md`: unchanged (installed by the runner, committed as-is). Not edited.
- **How verified:**
  - Build (`cmake --build build -j2`, DZ_WERROR=ON default): exit 0, no warnings.
  - `./build/op_tests` → 533 PASS, 0 FAIL, exit 0. Diff lines:
    ```
    PASS diff.unsupported_pattern_track_1
    PASS diff.clip_repeats_and_pattern_exact
    PASS diff.clip_repeats_and_pattern_replays
    PASS diff.clip_all_four_fields_exact
    PASS diff.clip_all_four_fields_replays
    PASS diff.clip_shrink_patterns_exact
    PASS diff.clip_shrink_patterns_replays
    PASS diff.clip_grow_patterns_exact
    PASS diff.clip_grow_patterns_replays
    PASS diff.duplicate_clip_no_dup_op
    PASS diff.duplicate_clip_replays
    PASS diff.reorder_clip_no_reorder_op
    PASS diff.reorder_clip_replays
    ```
    (The pre-existing `diff.*` checks, including the 6 golden round trips and `diff.example_text_exact`, still pass.)
  - `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures`, exit 0.
  - `bash harness/test_cli_errors.sh` → `PASS diff_clip_repeats_ok (exit 0)`, `test_cli_errors: 0 failures`, exit 0.
  - `UV_CACHE_DIR=<workspace>/.cache/uv bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`, exit 0.
  - Sanitizers: `cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`, `cmake --build build-san --target op_tests -j2`
    → no warnings; `./build-san/op_tests` → `ALL PASS (0 failures)`, 533 PASS, exit 0.
  - CLI round trip (repeats-only pair): `opd diff arranged.opj b.opj -o j.opjl` → `diff: 1 op(s)`, journal
    `{"index":2,"op":"set_clip_repeats","repeats":2}`; `opd apply arranged.opj j.opjl -o c.opj` is byte-identical
    (`cmp`) to the canonical `b` (canonicalized via `opd diff b b` + `opd apply b empty -o bcanon`).
  - Scope note: the acceptance criterion "any pair differing only in supported fields" is evidenced by the
    replay checks above (representative pairs, the 6 goldens, and duplicate/reorder round trips), not by an
    exhaustive or randomized property sweep.
- **PROPOSE / BLOCKED:** none. Nothing outside scope was changed.
