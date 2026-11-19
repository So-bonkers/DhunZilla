---
id: WP-0157
title: Autosave + crash recovery in the Document layer
phase: 1
status: ready
depends_on: [WP-0141, WP-0142]
estimate_agent_hours: 1.5
human_review: none
---

## Context — why this exists
The app must not lose work on a crash. This adds autosave and recovery to the JUCE-free `op::Document`
(`src/doc/document.{hpp,cpp}`, WP-0141) using `op::project_bytes` (WP-0142). The doc layer has **no timers
and no clock** (ADR-0004): the app will call `autosave()` periodically later. Recovery detection compares
**content**, never file times. No JUCE, no GUI here.

## Spec — what "correct" means
Add to `src/doc/document.hpp` / `document.cpp` (namespace `op`). **Use these names and signatures
exactly** — a hidden probe compiles against them. Existing members keep their behaviour.

```cpp
enum class RecoveryState { None, AutosaveNewer };

class Document {
public:
    // ... existing API unchanged ...
    static std::string autosave_path(const std::string& path);   // returns path + ".autosave"
    bool autosave(std::string& err);
    static RecoveryState recovery_state(const std::string& path);
    static bool open_recovered(const std::string& path, Document& out, std::string& err);
    // save() and save_as() also gain the autosave-removal rule below.
};
```

Files: autosave file = `autosave_path(path())`; its temp file = `autosave_path(path()) + ".tmp"`
(i.e. `<path>.autosave.tmp`).

### `bool autosave(std::string& err)` — evaluate these rules in this order
1. `path()` empty (untitled document) → return false, `err == "cannot autosave an untitled document"`.
   Nothing is written (even if dirty).
2. Document not `dirty()` → return true, nothing is touched (an existing autosave file stays as is), `err` unchanged.
3. `revision()` equals the revision at the last successful autosave of this Document object (and no
   open/open_recovered/save/save_as happened since) → return true, nothing touched (no write, `err` unchanged).
   Use a private member (e.g. `std::uint64_t autosaved_revision_` + `bool have_autosaved_`); `open`, `open_recovered`,
   `save` and `save_as` (success) reset it to "never autosaved".
4. Otherwise write `op::project_bytes(project())` **atomically**: write the bytes to the temp file
   (binary, truncating), flush/close and verify the stream is good, then `std::rename(temp, autosave file)`.
   Success → record the revision, return true.
   Any failure (temp cannot be opened/written, rename fails) → remove the temp file if it exists
   (`std::remove`, ignore its result), return false, `err == "cannot write autosave '<autosave file path>'"`,
   do **not** record the revision, and leave an already-existing autosave file untouched.
- `autosave` may be called during an active gesture: it writes the current `project()` (the preview).
- `autosave` **never touches the main file** `path()` in any case, and never changes `revision()`,
  `dirty()`, the history or `path()`.

### `save()` / `save_as()`
After the existing write **succeeds**, remove the autosave file of the (new) `path()` with `std::remove`
(missing file is fine; ignore the result) and also remove its `.tmp` file the same way; reset the
"last autosaved" state. If the save fails, the autosave file is left untouched. (`save_as` removes only the
autosave file belonging to the new path; the old path's autosave file is not touched.)

### `static RecoveryState recovery_state(const std::string& path)`
Returns `RecoveryState::AutosaveNewer` iff **all** hold: `<path>` loads with `op::load_project`; `autosave_path(path)`
loads with `op::load_project` (a missing, unreadable, corrupt or truncated autosave is simply not recoverable);
and `op::project_bytes` of the two loaded projects **differ** (canonical content comparison — no mtimes,
no clock, no raw byte comparison of the files). In every other case returns `RecoveryState::None`. It never
modifies or deletes any file and never throws.

### `static bool open_recovered(const std::string& path, Document& out, std::string& err)`
- If `recovery_state(path) != RecoveryState::AutosaveNewer` → return false, `out` unchanged,
  `err == "no recoverable autosave for '<path>'"`.
- Otherwise `out` becomes: `project()` = the project loaded from the autosave file; `path() == path`;
  `revision() == 0`; no undo/redo history; no gesture; **saved baseline = the on-disk main file**
  (`saved_bytes_ = op::project_bytes` of the project loaded from `<path>`), so `dirty()` is **true**
  (the content differs from the baseline); after a later `save()` the main file holds the recovered content
  and `dirty()` is false. `open_recovered` does not delete or modify the autosave file or the main file, and
  resets the "last autosaved" state.
- Plain `Document::open` is unchanged (ignores the autosave file, leaves it in place).

### Docs
Extend the header's top comment with a short autosave/recovery usage example. Do not edit `docs/spec/`.

## Acceptance criteria
- [ ] `grep -ri juce src/doc src/engine src/cli` → nothing; no `<chrono>`/`time(`/`clock` in `src/doc`.
      Builds with zero warnings (`-Werror`) and under `-DDZ_SANITIZE=ON`. No CMake change should be needed.
- [ ] Unit tests in `tests/test_main.cpp` (extend `test_document()` or add `test_document_autosave()` called from
      `main`; CHECK names prefixed `doc.autosave.`), at least 12 new CHECKs covering, using temp files copied from
      `tests/fixtures/goldens/demo.opj` (never write into `tests/fixtures`): untitled → exact error and no file;
      clean → no autosave file created; dirty → file created whose bytes == `op::project_bytes(project())` and no
      `.tmp` left; same revision → second call does not rewrite (delete the file by `std::remove`, call again → still
      absent, returns true); new revision → rewritten; main file bytes unchanged by autosave; failure with
      `<autosave>.tmp` pre-created as a **directory** → false, exact error, main file and prior autosave untouched;
      failure with the autosave path itself a directory → false and no `.tmp` left; `save()` removes the autosave;
      `recovery_state` None for no autosave / identical content / corrupt autosave, AutosaveNewer for differing
      content; `open_recovered` → dirty, project equals autosave content, then `save()` makes main file == recovered
      content and clean; `open_recovered` with no autosave → false, exact error, `out` unchanged.
- [ ] Standard gates green: `./build/op_tests`, `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`,
      `bash harness/test_glue_scripts.sh`; sanitizer build (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`) `op_tests` passes.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), `src/app/`, any JUCE/GUI/audio code, `src/engine/`
  (incl. `save_project`: do NOT change it; if you think it should be atomic write `PROPOSE:`), golden fixtures
  (`tests/fixtures/`), `ai-sidecar/`, `docs/`, `spikes/`. No timers, threads or clocks. Deleting stale autosave when
  a clean document is autosaved, and a recovery prompt UI, are deferred.

## Git
- Branch `wp-0157-autosave-recovery` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0157: Document autosave + crash recovery`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.** Build with `-j4` at most.

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/doc/document.hpp`: `enum class RecoveryState { None, AutosaveNewer }`; `Document::autosave(err)`,
    `static autosave_path`, `static recovery_state`, `static open_recovered`; private `autosaved_revision_` +
    `have_autosaved_`; autosave/recovery usage example added to the top comment.
  - `src/doc/document.cpp`: anonymous helpers (remove `<path>.autosave` and `.tmp`); `open`, `open_recovered`
    reset the autosave state; `save()`/`save_as()` remove the new path's autosave + tmp after a successful write;
    `autosave` implements the 4 rules in order (temp write + verify + `std::rename`, remove temp on failure);
    `recovery_state` compares `project_bytes` of the two loaded projects inside try/catch.
  - `tests/test_main.cpp`: new `test_document_autosave()` (called from `main` after `test_document()`), 43 CHECKs
    `doc.autosave.*`, working in temp files under `std::filesystem::temp_directory_path()`; `fixtures/` never written.
  - No CMake change, no `src/engine/` change, no `docs/spec/` change, no JUCE/clock/timer.
- **How verified:** (all run in this worktree; build dir `build/` at -j2, sanitizer build dir `build-asan/`)
  - Configure + build with -Werror (target-level `-Werror` from CMakeLists): `cmake --build build -j2` exit 0,
    `grep -n "Werror\|warning" /tmp/wp0157_build.log | wc -l` -> `0`.
  - Acceptance grep:
    ```
    $ grep -ri juce src/doc src/engine src/cli      -> (no output, exit 1)
    $ grep -n "<chrono>\|time(\|clock" src/doc/*
    src/doc/document.cpp:2:// No file I/O beyond load_project/save_project, no clock, no GUI. ...
    ```
    The only match is a comment that was already in HEAD (`git show HEAD:src/doc/document.cpp | grep -n clock`
    prints the same line 2). No `<chrono>`, `time(` or clock code.
  - Unit tests: `TMPDIR=/home/shubhankar_dgx/qwen-scratch/.cache/tmp ./build/op_tests` -> exit 0, last lines:
    ```
    INFO resample polyphase: 272007 float scalars bit-compared vs reference
    ALL PASS (0 failures)
    ```
    `grep -c '^PASS doc.autosave' ` on that log -> `43` (requirement: at least 12).
    One CHECK initially failed because my own test setup was wrong (the default Project has no tracks, so
    `set_track_gain`/`set_bpm` were rejected); the test now uses `add_track`. No implementation change was needed.
  - Goldens: `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/run_goldens.sh` ->
    `run_goldens: 8 fixtures, 0 failures` (exit 0).
  - CLI errors: `bash harness/test_cli_errors.sh` -> `test_cli_errors: 0 failures` (exit 0).
  - Glue: `bash harness/test_glue_scripts.sh` -> `test_glue_scripts: 0 failures` (exit 0).
  - Sanitizers: `cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`, build -j2 `op_tests` with no
    warnings; `./build-asan/op_tests` -> `ALL PASS (0 failures)`, `43` `doc.autosave` PASS lines, no ASAN/UBSAN reports.
- **Follow-ups noticed (not done):**
  - The existing `test_document()` writes to literal `/tmp/...` paths; the new test uses `temp_directory_path()`
    (honours `TMPDIR`). Harmonising the older tests is out of scope.
  - Per the spec's failure rule, the cleanup `std::remove(<autosave>.tmp)` also removes an *empty* directory
    planted at that path (a non-empty one is left alone). Behaviour is as specified; noted for awareness.
  - The deferred items (stale autosave deletion for clean docs, recovery prompt UI, app timer) are untouched.
- **PROPOSE / BLOCKED:** none.
