---
id: WP-0141
title: JUCE-free Document layer — open/save/dirty, undo/redo, one-undo-entry-per-gesture (WP-0137a)
phase: 1
status: ready
depends_on: [WP-0135, WP-0136]
estimate_agent_hours: 3
human_review: none
---

## Context — why this exists
WP-0137 (the JUCE app skeleton) is split in four. This is part (a): the **document object** the app
will own — everything a GUI needs from a project (open, save, dirty flag, undo/redo, slider
gestures) with **no JUCE and no GUI code**, so it is unit-testable in `op_tests` and can be reused
by the CLI later. The app (WP-0137b+) will only call this class. Builds on WP-0135 (`op::History`,
operations) and WP-0136 (mute/solo/master).

## Spec — what "correct" means
New files `src/doc/document.hpp` and `src/doc/document.cpp`, namespace `op`, in a new static library
CMake target **`dzdoc`** (links `opengine`; add it to `DZ_OUR_TARGETS` so `-Werror`/sanitizers apply;
`op_tests` links it). **Use these names and signatures exactly** — another package compiles against them:

```cpp
namespace op {

struct TrackRow {             // what a track-list row displays; derived from the model, never stored
    std::string name;
    float gain_db = 0.0f;
    float pan = 0.0f;
    bool mute = false;
    bool solo = false;
    bool audible = true;      // op::track_audible(project, i) — the WP-0136 rule
};

class Document {
public:
    // Load `path` with op::load_project. On failure: returns false, `out` unchanged, err set.
    // On success: out holds the project, path() == path, dirty() == false, no undo/redo history.
    static bool open(const std::string& path, Document& out, std::string& err);

    const Project& project() const;   // current state (includes an in-progress gesture preview)
    const std::string& path() const;
    std::uint64_t revision() const;   // +1 on EVERY change to project(); UI refreshes when it moves

    // Apply one WP-0135 operation (JSON object) through the owned op::History.
    // Refused (false, err "gesture active") while a gesture is active. Rejected op: false, project,
    // history and revision unchanged.
    bool apply(const nlohmann::json& op, std::string& err);
    bool undo();                      // false if nothing to undo or a gesture is active
    bool redo();                      // false if nothing to redo or a gesture is active
    bool can_undo() const;
    bool can_redo() const;

    // Continuous edits (slider drag). Exactly ONE undo entry per gesture, however many updates.
    bool begin_gesture();             // false if a gesture is already active
    // Preview: applies `op` to the state captured by begin_gesture() (NOT cumulatively on top of the
    // previous preview) — project() shows the result; history untouched. Invalid op: false, preview
    // unchanged. Calling it with no active gesture: false, err "no gesture".
    bool update_gesture(const nlohmann::json& op, std::string& err);
    // Commit: restore the begin state, then apply the LAST successful update op through History
    // (one undo entry). No successful update → no history entry, returns true. Ends the gesture.
    bool end_gesture(std::string& err);
    void cancel_gesture();            // restore the begin state, no history entry, end the gesture
    bool gesture_active() const;

    // dirty() == the canonical save bytes of project() differ from the bytes at the last open/save.
    // (Undoing back to the saved state makes it clean again.)
    bool dirty() const;
    bool save(std::string& err);                          // writes path() via op::save_project
    bool save_as(const std::string& path, std::string& err);  // on success path() becomes `path`

    std::vector<TrackRow> track_rows() const;
};

}  // namespace op
```
`revision()` rules: `apply`/`undo`/`redo` success → +1; successful `update_gesture` → +1;
`end_gesture` → +1 if it changed `project()` compared with the last preview, else unchanged;
`cancel_gesture` → +1 if it changed `project()`; failures never change it; `save` never changes it.
Doc comments with a short usage example on the class (same style as `ops.hpp`).

## Acceptance criteria
- [ ] `grep -ri juce src/doc src/engine src/cli` → nothing. `dzdoc` builds with zero warnings under the
      existing `-Werror` and under `-DDZ_SANITIZE=ON`.
- [ ] Unit tests in `tests/test_main.cpp` (`test_document()`, same CHECK style), at least:
      open demo.opj → not dirty, rev 0-or-stable; `set_track_gain` → dirty, can_undo; undo → bytes equal
      the original and **not dirty**; redo → bytes equal the edited state; rejected op → revision,
      bytes and can_undo unchanged; gesture with 50 `update_gesture` calls (gain −1, −2 … −50) →
      exactly one undo entry, final gain −50, one `undo()` restores the original bytes; update is not
      cumulative (two updates of `set_track_gain` −6 then −3 → −3, not −9); `cancel_gesture` restores
      and adds no entry; `apply`/`undo`/`redo` refused during a gesture; `save_as` to a temp file →
      not dirty, file bytes == `op::save_project` of `project()`; `track_rows()` reflects mute/solo and
      `audible` per the WP-0136 rule; `open` of a missing file → false, err non-empty.
- [ ] All existing checks green: `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`; sanitizer build
      (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`) `op_tests` pass.
- [ ] ROADMAP.md: WP-0141 row at the top of the work-package table.

## Out of scope — do NOT touch
- Any JUCE / GUI / audio code, `src/app/`, `spikes/`. Engine behavior, operations spec, model fields
  (if something is missing write `PROPOSE:`). Golden fixtures, `ai-sidecar/`, `docs/adr/`.

## Git
- Branch `wp-0141-document-model` from the current HEAD of this worktree (already the correct base — do not check out, pull or merge `main`). Commit: `WP-0141: JUCE-free Document (undo/redo, gestures, dirty)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.** Build with `-j4` at most.

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/doc/document.hpp` / `src/doc/document.cpp` (new): `op::Document` + `op::TrackRow` with the exact
    spec signatures. Dirty flag = canonical save bytes (`project_to_json(p).dump(2) + "\n"`, same writer as
    `op::save_project`) vs the bytes captured at open/save. Gestures: `begin_gesture` snapshots the begin
    state; each `update_gesture` previews by applying the op to that snapshot (non-cumulative, history
    untouched); `end_gesture` restores the snapshot and commits the last successful op through
    `op::History` → exactly one undo entry; `cancel_gesture` restores, no entry. Revision bumps follow the
    spec rules exactly (failures never bump; `save` never bumps).
  - `CMakeLists.txt`: new static lib `dzdoc` (links `opengine`), added to `DZ_OUR_TARGETS`; `op_tests` links it.
  - `tests/test_main.cpp`: `test_document()` — 85 CHECKs covering every acceptance bullet.
  - `ROADMAP.md`: WP-0141 row at the top of the work-package table.
- **How verified:** (commands + actual output)
  - `grep -ri juce src/doc src/engine src/cli` → no output, exit 1 (nothing). ✓
  - `-Werror` build (`DZ_WERROR:BOOL=ON` in build/CMakeCache.txt), `cmake --build build -j4`: zero warnings;
    `[100%] Built target op_tests` (dzdoc built at 84%).
  - `./build/op_tests` → exit 0, `ALL PASS (0 failures)` — 391 checks total, 85 of them `doc.*`, including:
    open demo.opj not dirty / rev 0; apply→dirty+can_undo+rev+1; undo→bytes equal original **and clean**;
    redo→bytes equal edited; rejected op→revision/bytes/can_undo unchanged; gesture with 50 updates
    (−1…−50) → `doc.gesture_exactly_one_entry` (one undo restores original bytes, then `can_undo()` false),
    final gain −50; non-cumulative (`−6` then `−3` → `−3`); cancel restores + no entry; apply/undo/redo
    refused during gesture (err `"gesture active"`); `update_gesture` with no gesture → err `"no gesture"`;
    save_as temp → clean, file bytes == `op::save_project` of `project()`; `track_rows()` reflects mute/solo
    and `audible` per WP-0136 (solo gates others, mute wins); open missing file → false + non-empty err;
    failed open on a loaded doc leaves it untouched.
  - `bash harness/run_goldens.sh` → `run_goldens: 7 fixtures, 0 failures` (incl. negative-control).
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  - `bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`. (First run in this session hit
    14 failures that were purely sandbox environment — uv could not write its cache under `~/.cache/uv`,
    "Read-only file system"; rerun with `UV_CACHE_DIR` pointed at a writable dir → 0 failures. No product change.)
  - Sanitizers: `cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san -j4`
    (zero warnings) then `./build-san/op_tests` → exit 0, `ALL PASS (0 failures)`.
- **Follow-ups noticed (not done):**
  - The CLI (`opd apply`) could adopt `Document` for its journaling later ("reusable by the CLI later");
    untouched here.
  - `Document` inherits `History::kCap = 256` silently; when the app lands (WP-0137b) it may want a way to
    surface "undo stack full". Left alone — spec fixed no such API.
- **PROPOSE / BLOCKED:** none.
