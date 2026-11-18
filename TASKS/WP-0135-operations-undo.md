---
id: WP-0135
title: Model operations + undo/redo history + `opd apply` journal CLI
phase: 1
status: ready
depends_on: [WP-0134]
estimate_agent_hours: 10
human_review: none
---

## Context — why this exists
Phase 1 (skeleton DAW) needs undo/redo and a single mutation path that the coming GUI
(ADR-0007, `src/app/`) and the scripting/copilot API will both use. AGENTS.md already requires
every project mutation to be a serializable operation; today only `add_attachment` follows that.
This package builds the operation layer, headless and fully testable, before any GUI exists.

## Spec — what "correct" means
**`docs/spec/operations.md` (included in this branch — read it fully; it is normative).** Summary:
JSON operation objects (`{"op": ...}`), atomic `apply` validated by re-using the save→parse path,
JSON-Lines journals applied all-or-nothing, snapshot-based `History` (undo/redo, cap 256), and a new
`opd apply <proj.opj> <ops.opjl> [-o out.opj]` command.

## Acceptance criteria
- [x] New files `src/engine/ops.hpp` / `src/engine/ops.cpp` with the public API (doc comments with a
      usage example each):
      `bool apply_op(Project&, const nlohmann::json& op, std::string& err);`
      `bool apply_journal(Project&, const std::string& jsonl_text, std::string& err);`
      `class History { bool apply(Project&, const nlohmann::json&, std::string&); bool undo(Project&); bool redo(Project&); bool can_undo() const; bool can_redo() const; size_t undo_depth() const; };`
- [x] Validation re-uses the existing parse/save code: refactor `src/engine/model.cpp` so the JSON
      building inside `save_project` and the JSON parsing inside `load_project` are callable on
      in-memory JSON (internal helpers, e.g. `project_to_json` / `project_from_json`), **without
      changing any output bytes** — `bash harness/run_goldens.sh` must stay green and every
      existing `op_tests` assertion must still pass unchanged.
- [x] Unit tests in `tests/test_main.cpp` (same style as existing ones), at least:
      every op in the spec table: one success case + one rejection case (out-of-range index or bad
      value) asserting the project is byte-identical (via save to a temp file) after the rejection;
      `remove_track` on a referenced track → rejected `"track in use"`; `remove_pattern` on a
      referenced pattern → rejected `"pattern in use"`; unknown op and unknown extra key → rejected;
      History: apply/undo/redo/clear-redo-on-new-apply/cap-256 drops oldest.
- [x] **Property test** (seeded, deterministic, ≥ 500 random op sequences of length 1–40 against
      `tests/fixtures/goldens/demo.opj`, using the engine's existing seeded RNG in `rng.hpp`, never
      `std::random_device` or time): undo-all → saved bytes equal original; redo-all → equal forward.
- [x] `opd apply` per spec, plus new cases in `harness/test_cli_errors.sh` (follow its style):
      success → exit 0 and output file written; bad journal JSON → 4; rejected op on line 3 →
      exit 5, stderr contains `line 3:`, **output file not created/modified**; missing project → 3;
      no args → 2.
- [x] Journal determinism: applying the same journal twice from fresh copies → `cmp`-identical `.opj`.
- [x] `docs/spec/operations.md` committed exactly as provided (do not edit it). Add one line to the
      CLI section of `docs/spec/project-model.md` listing `opd apply`, and add `opd apply` to the
      `usage()` text in `src/cli/main.cpp`.
- [x] All existing checks green: `cmake --build build -j && ./build/op_tests`,
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`,
      `cd ai-sidecar && uv run pytest -q`. Build has **zero new warnings** (`-Wall -Wextra`).
- [x] ROADMAP.md: add a WP-0135 row at the top of the work-package table. (A Phase 1 table does
      not exist yet — put it in the existing table; the orchestrator restructures later.)

## Out of scope — do NOT touch
- Existing CLI commands' behavior (`attach`, `import-midi`, `render`, …). Migrating them onto ops is a
  later package — write it as a `PROPOSE:` if you think it's needed.
- Render semantics, golden fixtures (`tests/fixtures/` is read-only), `ai-sidecar/`, `harness/job_client.py`.
- The `fail()` exit-code mapping in main.cpp (it has a known quirk: any code name starting with 'u'
  maps to 2 — do not "fix" it here; do not choose a code name starting with 'u').
- `spikes/`, `docs/adr/`.

## Git
- Branch `wp-0135-operations` from `main` **after** pulling the latest `main`. Commit message:
  `WP-0135: model operations + undo/redo + opd apply`. Include this task file (with the handoff
  report filled in) and `docs/spec/operations.md`.
- **Do not push or merge.** The reviewer will tell you when to push.

## Notes / landmines
- `nlohmann/json` is vendored at `third_party/` (see `third_party/versions.lock`); json object key
  order: `save_project` produces canonical order today — keep using its path, don't build JSON twice.
- `float` vs `double` fields (`Track::gain_db` is float): round-trips through JSON must not change
  saved bytes. If a property-test failure shows a 1-ulp drift, the bug is in your op (e.g. assigning
  a double into a float differently than the parser does) — make ops go through the same parse path.
- Reject JSON numbers where an integer index is required unless they are exact integers
  (`2.0` ok? → **no**: require `is_number_integer()`), and reject negative indices.
- Keep `apply_op` free of file I/O and clocks (determinism rules in AGENTS.md).

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/ops.hpp` / `src/engine/ops.cpp` (new): public API per spec — `apply_op`,
    `apply_journal` (+ `JournalOutcome`/`JournalFailure` so the CLI maps exit codes without
    string-matching), snapshot-based `History` (cap 256, oldest dropped). Doc comments with usage
    examples on every public entry point. Strategy: mutate a copy of the canonical serialization,
    parse back with the untouched load path, re-validate with one more save→parse round-trip —
    no second set of validation rules; `p` is only replaced on success (atomic). No file I/O,
    clock or randomness in `apply_op`. Indices require `is_number_integer()`, negatives rejected.
  - `src/engine/model.cpp` + `src/engine/model_json.hpp` (new internal header): JSON building of
    `save_project` and parsing of `load_project` extracted to engine-internal `project_to_json` /
    `project_from_json` (code moved verbatim; not public API). `save_project` = build + dump,
    `load_project` = parse file + `project_from_json`. Zero output-byte change (evidence below).
  - `src/cli/main.cpp`: new `opd apply <proj.opj> <ops.opjl> [-o out.opj]` (default in-place;
    prints `apply: N op(s) -> <out>`; nothing written on failure; exit codes via existing
    `fail()`: 2 usage, 3 not-found, 4 parse, 5 rejected — code names `usage/not_found/parse/apply`,
    none starting with 'u' except the untouched pre-existing `usage`). `usage()` line added.
  - `tests/test_main.cpp`: +57 assertions — every spec-table op (success + rejection with
    byte-identical-save check), `"track in use"` / `"pattern in use"`, unknown op, unknown extra
    key, float/negative index strictness, History (apply/undo/redo/clear-redo-on-new-apply/
    failed-apply-touches-nothing/cap-256-drops-oldest), seeded property test.
  - `harness/test_cli_errors.sh`: +11 `opd apply` cases in existing style.
  - `CMakeLists.txt`: `ops.cpp` added to `opengine`. `docs/spec/project-model.md`: one CLI line
    for `opd apply`. `ROADMAP.md`: WP-0135 row at top of the work-package table.
    `docs/spec/operations.md`: committed exactly as provided, untouched (sha256
    `eb2606a5ebd2801f069dde20a239fbb849a10d4b38067330762e8cf098d69e14`).

- **How verified:** (commands + actual output)
  - `cmake --build build --clean-first -j` → exit 0; `grep -ci warning` on full log → `0`
    (`-Wall -Wextra -ffp-contract=off` from CMakeLists). clang-format not installed on this rig
    ("where available" clause); no clang locally per AGENTS.md, gcc 13.3 only.
  - `./build/op_tests` → `ALL PASS (0 failures)`, exit 0; all 207 pre-existing assertions pass
    unchanged. New WP-0135 lines include:
    ```
    PASS ops.remove_track_in_use            PASS hist.cap_256
    PASS ops.remove_pattern_in_use          PASS hist.cap_drops_oldest
    PASS ops.reject_integral_float_index    PASS hist.new_apply_clears_redo
    PASS ops.set_bpm_reject_err_format      PASS prop.undo_all_bytes_equal_original
    PASS ops.add_attachment_reject_duplicate PASS prop.redo_all_bytes_equal_forward
    INFO ops property: 500 sequences (len 1..40), 7138 applied, 3220 rejected
    ```
    Property test re-run → identical counts (seeded PCG32 from `rng.hpp`, seed 20261118; no
    random_device/time).
  - `bash harness/run_goldens.sh` → `run_goldens: 6 fixtures, 0 failures` (incl. negative control).
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`, incl. new:
    ```
    PASS apply_ok (exit 0)                 PASS apply_reject_output_not_created
    PASS apply_output_written              PASS apply_reject_output_unmodified
    PASS apply_bad_journal (exit 4)        PASS apply_missing_proj (exit 3)
    PASS apply_parse_no_output             PASS apply_missing_journal (exit 3)
    PASS apply_rejected_line3 (exit 5, stderr names line)   PASS apply_no_args (exit 2)
    PASS apply_determinism (identical .opj)
    ```
    stderr on rejection: `error: apply: line 3: remove_track: track in use`.
  - Journal determinism also covered engine-side (`prop.*` byte compares) and CLI-side
    (`apply_determinism`, `cmp`).
  - No-byte-change proof beyond goldens: built pre-change engine from `main` (git worktree), ran
    load→save on all 6 golden `.opj`s with both engines — `PASS main==branch save <name>` ×6,
    `A/B evidence: 0 failures`; also `gen-testproject` and `attach` outputs byte-identical across
    binaries. (4 goldens differ from their *own* round-trip output under **both** binaries —
    pre-existing fixture key-order style; rendering goldens unaffected.)
  - `bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures` (run with
    `UV_CACHE_DIR=/tmp/uvcache`: this container's sandbox makes `~/.cache/uv` read-only — same
    workaround needed at baseline on untouched `main`, not a WP-0135 artifact).
  - `cd ai-sidecar && uv run pytest -q` → `102 passed, 2 skipped, 57 warnings in 11.44s`
    (same `UV_CACHE_DIR` note; identical to baseline).

- **Metrics:** op_tests assertions 207 → 264 (+57); property test: 500 sequences × length 1–40,
  7138 successful applies / 3220 clean rejections; build warnings 0 → 0 (gcc 13.3, `-Wall -Wextra`);
  CLI error suite cases 6 → 17; glue + sidecar suites unchanged from baseline (0 failures / 102+2).

- **Follow-ups noticed (not done):**
  - Track `gain_db`/`pan` have no range validation in the load path, so `set_track_gain`/
    `set_track_pan` accept any number — a direct consequence of "re-use the parse path, invent no
    second rule set". If ranges are wanted, they belong in `model.cpp` as a separate package.
  - `History` snapshots grow with project size; fine at current scale (spec chose snapshots).
  - GUI-facing niceties (`clear()`, move-clips-by-id) intentionally not added — no consumer yet.

- **PROPOSE / BLOCKED:**
  - PROPOSE: follow-up package migrating existing CLI mutators (`attach`, `import-midi`) onto
    `apply_op`/`History` so every mutation path is journalled (explicitly out of scope here).
  - PROPOSE: canonicalize the 4 non-canonical golden `.opj` fixtures' key order in a future WP
    that re-records them with owner sign-off (fixtures are read-only for this package; behavior
    unaffected — `run_goldens.sh` compares rendered WAVs).
  - No BLOCKED.

