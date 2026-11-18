---
id: WP-0142
title: One canonical serializer for project bytes (op::project_bytes)
phase: 1
status: ready
depends_on: [WP-0141]
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
The on-disk project format is currently defined twice: `op::save_project` in `src/engine/model.cpp`
writes `project_to_json(p).dump(2) + "\n"`, and `src/doc/document.cpp` has a private
`canonical_bytes()` that re-implements the same expression under a comment "keep in sync with
model.cpp::save_project". Two definitions of the file format will drift. This WP makes the engine own
exactly one function that produces the bytes, and has everything else call it. **Zero byte change to
any saved file.**

## Spec — what "correct" means
1. In `src/engine/model.hpp` (namespace `op`, next to `save_project`) declare, with a doc comment:
   ```cpp
   // The exact bytes save_project writes for `p`: canonical JSON (stable key order) dumped with
   // indent 2 plus one trailing '\n'. Pure function of `p`; the single definition of the file format.
   std::string project_bytes(const Project& p);
   ```
   Define it in `src/engine/model.cpp` as `project_to_json(p).dump(2) + "\n"`.
2. `save_project` keeps its signature, error string (`cannot write '<path>'`), binary-mode ofstream and
   return values, but writes `project_bytes(p)` (e.g. `f << project_bytes(p);`). It must no longer call
   `dump(` itself.
3. `src/doc/document.cpp`: delete the anonymous-namespace `canonical_bytes()` and its "keep in sync"
   comment; replace every use with `project_bytes(...)`. After this change `src/doc/` must contain **no
   `dump(` call and no `canonical_bytes`**, and `document.cpp` should drop the now-unneeded
   `#include "engine/model_json.hpp"` if nothing else in it needs it. Update the comments in
   `document.cpp` / `document.hpp` that say "canonical save bytes" to mention `op::project_bytes`.
   `Document`'s public API and semantics are unchanged. **Do not add caching to `dirty()`** (decided:
   out of scope; keep the API exactly as is).
4. Output of `opd` (apply, import-midi, render, etc.) must stay byte-identical: do not change
   `project_to_json`, key order or formatting.
5. Tests (`tests/test_main.cpp`, new `test_project_bytes()` called from `main()`, same CHECK style;
   check names start with `projbytes.`):
   - For **every** `*.opj` in `tests/fixtures/goldens/` (list: arranged, demo, imported, mixer,
     panorama, pitched, resampled, tempo — hard-code the list, paths via `OP_TEST_DATA`): load it,
     `save_project` to a `/tmp/op_projbytes_<name>.opj`, read that file back as binary, and CHECK it
     equals `op::project_bytes(loaded)` (`projbytes.<name>_matches_save`). Note: some goldens are
     older, non-canonical formatting on disk, so do NOT compare against the golden file itself.
   - CHECK `project_bytes` of a default-constructed `op::Project` is non-empty, ends with `"\n"`, and
     is identical across two calls.
   - CHECK that `Document` agrees: open demo.opj, `save_as` a temp file, file bytes ==
     `op::project_bytes(doc.project())`.

## Acceptance criteria
- [x] `grep -rn 'dump(' src/doc` prints nothing; `grep -rn canonical_bytes src` prints nothing;
      `grep -c 'dump(' src/engine/model.cpp` is 1.
- [x] `op::project_bytes` declared in `src/engine/model.hpp` with exactly the signature above.
- [x] Unit tests above pass (≥ 10 new CHECKs). → 12 new CHECKs, all pass.
- [x] All existing checks green: `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`; build has zero warnings
      (`-Werror`); sanitizer build (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`) `op_tests` pass.
- [x] Do NOT edit `ROADMAP.md`. (Untouched — see `git show --stat` in the handoff.)

## Out of scope — do NOT touch
- `ROADMAP.md`, golden fixtures (`tests/fixtures/`), `project_to_json` / the file format, `ops.cpp`,
  `ai-sidecar/`, `docs/adr/`, `spikes/`, any JUCE/GUI code, `dirty()` caching.

## Git
- Branch `wp-0142-project-bytes` from `main`. Commit: `WP-0142: one canonical serializer for project bytes`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.** Build with `-j4` at most.

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp`: declared `std::string project_bytes(const Project& p);` next to
    `save_project`, with the spec'd doc comment (single definition of the file format).
  - `src/engine/model.cpp`: defined `project_bytes` as `project_to_json(p).dump(2) + "\n"`;
    `save_project` keeps signature/error string/binary ofstream/return values and now writes
    `f << project_bytes(p);` — no `dump(` left in it (exactly 1 in the whole file, inside `project_bytes`).
  - `src/doc/document.cpp`: deleted the anonymous-namespace `canonical_bytes()` + "keep in sync"
    comment and the `engine/model_json.hpp` include; all 9 call sites now use `op::project_bytes`.
    Header comment updated to name `op::project_bytes`.
  - `src/doc/document.hpp`: the three "canonical save bytes" comments (header, `dirty()`,
    `saved_bytes_`) now reference `op::project_bytes`. Public API unchanged; no `dirty()` caching.
  - `tests/test_main.cpp`: new `test_project_bytes()` called from `main()` — 12 CHECKs named
    `projbytes.*` (8 goldens round-trip vs `save_project`, default-Project non-empty/ends `\n`/stable,
    `Document::save_as` agreement). Temp files `/tmp/op_projbytes_*`.
- **How verified:** (commands + actual output)
  - Grep gates:
    ```
    $ grep -rn 'dump(' src/doc          → (no output, exit 1)
    $ grep -rn canonical_bytes src      → (no output, exit 1)
    $ grep -c 'dump(' src/engine/model.cpp → 1
    ```
  - Clean full rebuild (`rm -rf build && cmake -S . -B build && make -C build -j4`): exit 0,
    `grep -icE 'warning|error'` over the whole build log = **0** (DZ_WERROR=ON by default).
  - `./build/op_tests` → `ALL PASS (0 failures)` (exit 0; 423 CHECKs total, of which 12 new):
    ```
    PASS projbytes.arranged_matches_save   PASS projbytes.demo_matches_save
    PASS projbytes.imported_matches_save   PASS projbytes.mixer_matches_save
    PASS projbytes.panorama_matches_save   PASS projbytes.pitched_matches_save
    PASS projbytes.resampled_matches_save  PASS projbytes.tempo_matches_save
    PASS projbytes.default_nonempty        PASS projbytes.default_ends_with_newline
    PASS projbytes.stable_across_calls     PASS projbytes.document_save_as_agrees
    ```
  - `bash harness/run_goldens.sh` → 8× `PASS <name>: deterministic + golden match` +
    `PASS negative-control (corrupt golden rejected)` → `run_goldens: 8 fixtures, 0 failures`.
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures` (incl. `apply_determinism (identical .opj)`).
  - `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh`
    → 14 PASS lines → `test_glue_scripts: 0 failures`.
  - Sanitizers: `cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san -j4`
    (zero warning/error lines) → `./build-san/op_tests` → `ALL PASS (0 failures)` (exit 0).
  - Zero-byte-change proof: built pre-WP `opd` from `main` in a temp worktree and diffed outputs of
    the new binary vs old, same inputs — `apply` (demo.opj + set_bpm journal), `import-midi`
    (two_notes.mid) and `render --seed 7` for all 8 goldens: **every output file byte-identical**
    (`cmp` clean; temp worktree removed afterwards).
- **Follow-ups noticed (not done):**
  - `tests/test_main.cpp::test_document()` has a local comment "Canonical save bytes of a Project —
    same writer as save_project" above its `bytes_of` lambda; still accurate, left untouched (out of scope).
- **PROPOSE / BLOCKED:** none.

