---
id: WP-0147
title: Arrangement clip operations — set_clip_repeats, set_clip_pattern, duplicate_clip, reorder_clip
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
The operation layer (WP-0135, `docs/spec/operations.md`) already has `add_clip`, `move_clip`,
`set_clip_gain`, `remove_clip`. A GUI/copilot arranging a beat also needs to change a clip's repeat
count and pattern, duplicate a clip, and change clip declaration order (which is render accumulation
order). This package adds exactly those four ops to `src/engine/ops.cpp`. No new undo machinery:
`History` snapshots whole projects, so undo/redo works automatically.

## Spec — normative
`docs/spec/operations.md` has ALREADY been replaced with the version containing the new section
"Clip operations — additive (WP-0147)". **Read it first and implement it exactly. Do not edit it.**
Summary (the spec wins on any conflict):

| op | JSON |
|---|---|
| `set_clip_repeats` | `{"op":"set_clip_repeats","index":1,"repeats":3}` |
| `set_clip_pattern` | `{"op":"set_clip_pattern","index":0,"pattern":2}` |
| `duplicate_clip` | `{"op":"duplicate_clip","index":0,"start_bar":4.0}` (copy inserted at index+1) |
| `reorder_clip` | `{"op":"reorder_clip","index":0,"to":2}` (final position of the moved clip) |

Implementation notes (follow the existing code in `src/engine/ops.cpp`):
- Add four `else if (name == "...")` branches in `apply_named`, next to `remove_clip`, using the existing
  helpers `check_keys`, `arg_index`, `has_number`. Clip count `n_clips` is computed the same way the
  existing clip ops do (`j.contains("arrangement") ? size : 0`).
- Need an integer-typed check for `repeats`/`pattern`: `is_number_integer()` else
  `'<key>' must be an integer`; missing → `missing argument '<key>'`. If the integer does not fit in
  `int` (use `long long`/`uint64_t` as `arg_index` does), do NOT store it; set the err to the model's
  range message from the spec (`clip.repeats out of range [1..4096]` / `clip.pattern out of range: <value>`)
  and return false. Otherwise write the JSON value and let the model parse (the existing final
  `project_from_json` step) do the range validation — do not duplicate `[1..4096]` logic beyond that.
- `duplicate_clip`: `j["arrangement"].insert(j["arrangement"].begin() + idx + 1, copy)` where `copy` is
  the clip JSON with `start_bar` replaced.
- `reorder_clip`: take the element out (`json c = arr[idx]; arr.erase(idx);`), then insert at `to`.
  `to` is checked with `arg_index` against `n_clips`.
- No changes to `History`, `apply_journal`, CLI, or `model.*`.

## Acceptance criteria
- [ ] `docs/spec/operations.md` unchanged by you (the runner installs it; `git diff` shows it only if the
      runner committed it — do not hand-edit).
- [ ] Unit tests added to `tests/test_main.cpp` (≥ 12 new `CHECK`s), covering on an arrangement of 3 clips:
      each new op's success effect (and that no other field changed); `set_clip_repeats` 0 / 4097 /
      `3.0` / missing key rejected with the exact strings; `set_clip_pattern` -1 / out-of-range rejected;
      `duplicate_clip` copies `gain_db`; `reorder_clip` index→0, index→last, `to==index`; bad `index`
      (e.g. 3 and -1) rejected for all four; every rejection leaves the project's saved bytes identical;
      `History` undo/redo round-trip restores/re-applies byte-exact `save_project` output for each op.
- [ ] A journal of mixed new ops via `op::apply_journal` is all-or-nothing (line N failure → unchanged).
- [ ] Builds with 0 warnings (`-Werror` on), `op_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh` all green; sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` green. Build with `-j4` max.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds rows at ship time), `docs/spec/*` (spec is supplied),
  `tests/fixtures/` (read-only), `src/cli/`, `src/engine/model.*`, `ai-sidecar/`, `docs/adr/`.
- Splitting/trimming clips, clip offsets, undo changes, new CLI subcommands.

## Git
- Branch `wp-0147-clip-ops` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0147: arrangement clip operations (set_clip_repeats, set_clip_pattern, duplicate_clip, reorder_clip)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/ops.cpp`: four new `else if` branches in `apply_named` (after `remove_clip`):
    `set_clip_repeats`, `set_clip_pattern`, `duplicate_clip`, `reorder_clip`. Two small helpers added
    in the anonymous namespace: `has_integer` (integer-typed, missing/mistyped messages) and
    `fits_int` (rejects values outside 32-bit `int` before they reach the JSON edit, so they can never
    wrap). Range checks for repeats/pattern are left to `project_from_json`, as the spec requires.
    `<cstddef>` added for `std::ptrdiff_t`.
  - `tests/test_main.cpp`: new `saved_text()` helper (exact `save_project` bytes) and a WP-0147 block
    inside `test_ops()` with 26 new `CHECK`s (success effects, exact error strings, no-wrap for
    `4294967297`, bad-index for all four ops, byte-exact History undo/redo for each op, and journal
    all-or-nothing plus success).
  - `docs/spec/operations.md`: unchanged by me (runner-installed; sha256
    `f9c298d8fcd9a7fb8c29dd7119cca7b96279ff99b1c1dca8a0d92324868568b4`), committed as-is.
  - No changes to `History`, `apply_journal`, CLI, `model.*`, fixtures, ROADMAP, or ADRs.
- **How verified:** (build dir `build/`, Release, `cmake --build build -j4`, `-Werror` on by default)
  - Build: 0 warnings, all targets built (`[100%] Built target op_tests`).
  - `./build/op_tests` → `437` PASS lines, `0` FAIL; last line `ALL PASS (0 failures)`. The 26 new
    WP-0147 checks include: `ops.set_clip_repeats`, `ops.set_clip_repeats_reject_zero`,
    `ops.set_clip_repeats_reject_4097`, `ops.set_clip_repeats_reject_no_wrap`,
    `ops.set_clip_repeats_reject_float`, `ops.set_clip_repeats_reject_missing`, `ops.set_clip_pattern`,
    `ops.set_clip_pattern_reject_negative`, `ops.set_clip_pattern_reject_oob_pattern`,
    `ops.set_clip_pattern_reject_no_wrap`, `ops.duplicate_clip`, `ops.duplicate_clip_copies_gain`,
    `ops.duplicate_clip_keeps_others`, `ops.duplicate_clip_reject_type`,
    `ops.duplicate_clip_reject_negative`, `ops.reorder_clip_to_last`, `ops.reorder_clip_to_first`,
    `ops.reorder_clip_to_self`, `ops.reorder_clip_reject_to_oob`, `ops.clip_ops_reject_bad_index`,
    `hist.set_clip_repeats`, `hist.set_clip_pattern`, `hist.duplicate_clip`, `hist.reorder_clip`,
    `ops.journal_clip_ops_all_or_nothing`, `ops.journal_clip_ops_applied` — all PASS.
  - `harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures`.
  - `harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  - `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv harness/test_glue_scripts.sh` →
    `test_glue_scripts: 0 failures`.
  - Sanitizers: `cmake .. -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`, `cmake --build build-san -j4 --target op_tests`
    → built; `./op_tests` → `ALL PASS (0 failures)`, zero `runtime error` / `AddressSanitizer` lines.
  - Acceptance-criteria note: "every rejection leaves saved bytes identical" is enforced by
    `rejected_unchanged` on every rejection `CHECK` (the duplicate, reorder, repeats, pattern ones).
  - Existing test helpers write their byte comparisons to `/tmp` (pre-existing convention); my
    `saved_text` follows that convention.
- **PROPOSE / BLOCKED:** none. `PROPOSE:` (non-blocking, outside scope): `apply_named` now has 23
  name-dispatch branches in one function; a future WP could split ops by family, but I did not touch it.
