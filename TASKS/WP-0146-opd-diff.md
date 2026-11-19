---
id: WP-0146
title: `opd diff a.opj b.opj -o edits.opjl` — emit an operations journal that transforms a into b
phase: 1
status: ready
depends_on: [WP-0135]
estimate_agent_hours: 1.5
human_review: none
---

## Context — why this exists
WP-0135 made every project mutation a journal of JSON operations (`opd apply`). The inverse tool is
missing: given two project files, produce the journal between them ("what did the AI/user change?",
reviewable edit lists, replaying a change onto another copy). **No new operations are added** — the diff
only uses the existing ones, so only differences those operations can express are supported; anything
else is a clean error.

## Spec — what "correct" means
`docs/spec/operations.md` has ALREADY been replaced with the version containing the new section
"`opd diff` — journal that transforms one project into another (additive, WP-0146)". **Read it first and
implement it exactly; it wins on any conflict.** It contains the exact op ordering, output format,
unsupported-field list and exit codes. Do not edit that file.

Implement exactly this:
1. `src/engine/ops.hpp` / `src/engine/ops.cpp` (namespace `op`; add `#include <vector>` to the header):
   ```cpp
   bool diff_projects(const Project& a, const Project& b, std::vector<nlohmann::json>& ops, std::string& err);
   ```
   with a doc comment + usage example like the neighbouring functions. Suggested approach: build
   `project_to_json(a)` and `project_to_json(b)` (from `engine/model_json.hpp`) and compare/emit from those
   JSON values; construct each op as `{"op": name, ...}`; at the end apply your ops with `apply_op` to a copy
   of `a` and compare `project_to_json(copy)` with b's (the safety net in the spec). `ops` is cleared at the
   start and left empty on failure. Error string format: `unsupported difference: <field>` with the field
   spelled exactly as in the spec (`seed`, `tracks[1].voice`, `patterns[0].track`, `arrangement[2].repeats`,
   `attachments`, ...; indices are 0-based decimal).
2. `src/cli/main.cpp`: new subcommand `diff` (add one usage line; dispatch next to `apply`):
   `opd diff <a.opj> <b.opj> -o <edits.opjl>`. Load both with `op::load_project` (missing file → `fail("not_found", path)`,
   load error → `fail("parse", err)`), call `diff_projects`, on false `fail("diff", err)` (→ exit 5, stderr
   `error: diff: unsupported difference: seed`). Only after success open the output and write each op as
   `op.dump() + "\n"` (a zero-byte file when there are no ops); then print `diff: N op(s) -> <out>`.
   Missing `-o` or unknown arg → `usage()` + `fail("usage", ...)` (exit 2). An unwritable output →
   `fail("write", ...)`. On every failure the output path must not be created or modified.
3. Unit tests in `tests/test_main.cpp` (`test_opd_diff()`, existing CHECK style, wired into `main`), at least
   10 new CHECKs: (a) identical projects → success and empty ops; (b) for each golden in
   `tests/fixtures/goldens/` (demo, mixer, arranged, panorama, pitched, tempo): apply a hand-made journal of
   several supported ops (bpm, master gain, track gain/pan/mute/solo, rename, add_track, add_note,
   remove_note, add_pattern, add_clip, move_clip, set_clip_gain, add_attachment) with `apply_journal` to get b,
   then `diff_projects(a, b)` → applying the ops gives a project whose `project_to_json(...).dump()` equals b's;
   (c) the exact text of the three-op example below; (d) the order rules (removals descending; remove_clip
   before remove_pattern before remove_track); (e) `seed` / `sample_rate` / `tracks[0].voice` / `tempo_map`
   changes → false, `ops` empty, exact `unsupported difference: <field>` strings; (f) when both seed and
   `beats_per_bar` differ, `seed` is reported (fixed order). Tests may `#include "engine/model_json.hpp"` for
   `op::project_to_json` (compare `.dump()` strings).

Exact example (a = `demo.opj`; b = a after `set_bpm 90`, `set_master_gain -3`, `set_track_gain index 0 -2.5`)
→ journal text, byte for byte:
```
{"bpm":90.0,"op":"set_bpm"}
{"gain_db":-3.0,"op":"set_master_gain"}
{"gain_db":-2.5,"index":0,"op":"set_track_gain"}
```

## Acceptance criteria
- [ ] For any supported pair, `opd apply a.opj edits.opjl -o c.opj` gives `c.opj` byte-identical to
      `save_project(b)` (the bytes `opd apply`/`opd attach` write for b), and diff of identical files is a
      zero-byte journal, exit 0; output is deterministic (same inputs → same bytes).
- [ ] Unsupported difference → exit 5, stderr `error: diff: unsupported difference: <field>`, output file not
      created (and an existing one left untouched). Other exit codes per the spec section.
- [ ] No new operation, no change to existing operation behaviour, `apply_op` / `apply_journal` untouched.
- [ ] Gates: build with 0 warnings (`-Werror` default), `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`; sanitizer build
      (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`) `op_tests` pass. Build with `-j4` at most.
- [ ] Add a few `opd diff` lines to `harness/test_cli_errors.sh` (usage 2, missing file 3, unsupported 5,
      success 0) in its existing `check` style.
- [ ] ROADMAP.md: NOT edited (the elephant adds the row).

## Out of scope — do NOT touch
- New operations (e.g. voice edits, clip repeats, tempo_map, attachments removal, `seed`/`sample_rate`
  setters) — they are exactly what makes a difference "unsupported". `ROADMAP.md`, `docs/spec/*`,
  `docs/adr/`, golden fixtures, `ai-sidecar/`, `src/doc/`, render/model code.

## Git
- Branch `wp-0146-opd-diff` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0146: opd diff — journal that transforms one project into another`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/ops.hpp` / `src/engine/ops.cpp`: `op::diff_projects` (+ `<vector>` include, doc comment with example). Emits ops in the spec's fixed order from canonical JSON of a and b; unsupported-field checks in spec order; safety-net replay of its own journal on a copy of `a`.
  - `src/cli/main.cpp`: `opd diff <a.opj> <b.opj> -o <edits.opjl>` (usage line, `cmd_diff`, dispatch next to `apply`). Output file opened only after a successful diff.
  - `tests/test_main.cpp`: `test_opd_diff()` (17 CHECKs: identical, 6 golden round-trips, exact spec example text, removal order exact + replay, 5 unsupported-field cases with exact strings, seed-before-beats_per_bar order), `#include "engine/model_json.hpp"`, wired into `main`.
  - `harness/test_cli_errors.sh`: 9 `opd diff` checks (usage 2, missing file 3, identical 0 + zero-byte, ok 0 + byte-identical apply round-trip, unsupported 5 with exact stderr and no output file).
  - `docs/spec/operations.md`: unchanged (installed by the runner; committed as-is).
  - No new operation; `apply_op` / `apply_journal` untouched. ROADMAP.md not edited.
  - Source for ops/CLI was adopted from the `salvage/WP-0146-opd-diff-reboot` commit after review against this task file and the spec (verified: canonical JSON always emits the fields it reads with `.at()`, so no throw paths).
- **How verified:**
  - `cmake -S . -B build && cmake --build build -j4` → 0 warnings with `-Werror`, all targets built.
  - `./build/op_tests` → `ALL PASS (0 failures)`; 428 PASS lines; `diff.*` lines all PASS (e.g. `PASS diff.example_text_exact`, `PASS diff.removal_order_exact`, `PASS diff.unsupported_seed`).
  - `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-asan -j4 --target op_tests && ./build-asan/op_tests` → `ALL PASS (0 failures)`, exit 0.
  - `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures`.
  - `bash harness/test_cli_errors.sh` → `PASS diff_no_args (exit 2)`, `PASS diff_missing_out (exit 2)`, `PASS diff_not_found (exit 3)`, `PASS diff_identical_ok (exit 0)`, `PASS diff_identical_zero_bytes`, `PASS diff_ok (exit 0)`, `PASS diff_roundtrip_byte_identical`, `PASS diff_unsupported (exit 5, stderr names field)`, `PASS diff_unsupported_no_output`, `test_cli_errors: 0 failures`.
  - `UV_CACHE_DIR=<workspace>/.cache/uv bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - Manual CLI run (demo.opj vs. demo after the spec's three ops):
    ```
    $ opd diff demo.opj b.opj -o edits.opjl
    diff: 3 op(s) -> .../edits.opjl            (exit 0)
    {"bpm":90.0,"op":"set_bpm"}
    {"gain_db":-3.0,"op":"set_master_gain"}
    {"gain_db":-2.5,"index":0,"op":"set_track_gain"}
    $ opd apply demo.opj edits.opjl -o c.opj && cmp c.opj b.opj   -> identical
    $ opd diff demo.opj mixer.opj -o unsup.opjl
    error: diff: unsupported difference: seed                  (exit 5, unsup.opjl not created)
    $ opd diff demo.opj demo.opj -o same.opjl
    diff: 0 op(s) -> .../same.opjl  (exit 0, 0 bytes)
    $ opd diff demo.opj b.opj -o nodir/x.opjl
    error: write: cannot open '.../nodir/x.opjl' for writing   (exit 5)
    ```
- **Follow-ups noticed (not done):**
  - Spec does not say whether `diff` should also reject a missing `-o` argument's value when the output path is itself a directory; currently it surfaces as `write` (exit 5), which matches the spec's write-failure rule.
- **PROPOSE / BLOCKED:** none.

## Note from the runner — prior attempt
An earlier run of this task was interrupted by a machine crash (not a defect). Its uncommitted partial work
is on branch `salvage/WP-0146-opd-diff-reboot` (ops.{hpp,cpp}, CLI wiring, spec section). You may inspect it
(`git show salvage/WP-0146-opd-diff-reboot -- <path>`) and reuse what is correct, but verify everything against
this task file — it was unfinished and untested. Create your branch as the Git section says; never commit
onto the salvage branch.
