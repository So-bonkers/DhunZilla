---
id: WP-0212
title: App open/new - File menu New / Open... / Open Recent (last 8 paths in an app-settings file) plus the `new`, `open <path>` and `discard yes|no` script commands
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: owner-screenshots
touches: [app, doc]
---

## Context - why this exists

Phase 1 needs a user to get a project INTO the app: File > New, File > Open (a `juce::FileChooser`
offering `.opj` and `.opd`) and an Open Recent submenu that remembers the last 8 paths. All of it must
be drivable headless, so the same code paths also get `--script` commands `new`, `open <path>` and
`discard yes|no`. Everything still goes through `op::Document` (one undo entry per gesture, never a
direct model mutation), and a failed open must never damage the document you already have.

Existing code this package reuses (read it first, do not reimplement it): `op::Document`
(`open`, `dirty`, `project()`, `path()`, `track_rows()`) in `src/doc/document.{hpp,cpp}`;
`op::load_project` / `op::new_project` / `op::NewProjectSpec` / `op::project_bytes` in
`src/engine/model.hpp`; `dz::MainComponent` and `dz::windowTitleFor` in `src/app/main_component.{hpp,cpp}`;
`dz::PlaybackController::stop()` in `src/app/playback_controller.hpp`. Read the whole script section of
`src/app/main.cpp` (grammar, `MainWindow` menu model, `App` modes) before editing - you extend it in
its existing style.

## Spec - normative (this task file is the contract)

### Files (exactly these 3; stage no others, except this task file)
- `src/doc/document.hpp`, `src/doc/document.cpp` - additive only: ONE new public member and its
  definition. `src/doc/document.cpp` is already in the `dzdoc` target.
- `src/app/main.cpp` - additive edits: script grammar, menu items, settings file, document switch.
- No new files, no `CMakeLists.txt` change, no new dependencies, no changes to `src/app/main_component.*`,
  `src/engine/`, `src/cli/`, `tests/`, `harness/`, `docs/`.

### Document addition (`namespace op`)
```cpp
// Replace *this with a fresh document holding `p` at `path` ("" = untitled): clean baseline, no
// undo/redo history, revision 0, no active gesture. A document SWITCH, not an edit: nothing goes
// through History.
void reset_to(const Project& p, const std::string& path);
```
It sets exactly: `project_ = p`, `path_ = path`, `revision_ = 0`, `history_ = History()`,
`saved_bytes_ = project_bytes(project_)`, `gesture_active_ = false`, `have_autosaved_ = false`.
After it: `dirty() == false`, `can_undo() == false`, `can_redo() == false`, `path()` == `path`.
Do NOT change `open()`, `save()`, `save_as()` or any other member.

### App: the ONE document switch (`src/app/main.cpp`)
Add to `class App` (names below are required, keep them exactly):
```cpp
int switch_document(bool is_new, const std::string& path, bool discard_ok, std::string& err);
```
Fixed order, no other effects:
1. **Guard:** if `doc_->dirty()` and `discard_ok` is false, return 1 without touching ANYTHING
   (document, component, playback, recent list all unchanged).
2. If a `PlaybackController` exists, call `playback_->stop()` (headless `--script`/`--snapshot`
   modes have no controller, so nothing to stop there).
3. If `is_new`: `doc_->reset_to(op::new_project(op::NewProjectSpec{}), "")` - the untitled document
   holds EXACTLY the project `opd new <out>` writes (byte-identical `op::project_bytes`; `opd new`
   defaults are bpm 120, 4 bars, 4/4, 48 kHz, one `"Synth"` track + one empty pattern + one clip).
   A default-constructed `op::Project{}` is NOT acceptable (it has no tracks and 0 bars).
4. Else: `op::Document::open(path, *doc_, err)` - that is `op::load_project` into a fresh document
   state (clean, no undo/redo history). On failure return 2 with `err` = the loader error
   VERBATIM (the exact string `op::Document::open` produced, no prefix, no rewording); `*doc_`, its
   component and the recent list stay exactly as they were.
5. Rebuild the component (see below), and for an open record the path in the recent list (see below).
Return 0 when the document now holds the new project.

### Script commands (extend the existing grammar; every existing command, message and exit code stays
byte-identical)
```
new                - File > New.            parse error, exact: 'new' takes no arguments
open <path>        - File > Open with the path given directly (no chooser).
                     `open` requires EXACTLY ONE token after it (the path; a path with spaces is
                     not supported): zero or two-plus tokens is the parse error, exact:
                                            'open' expects '<path>'
discard yes|no     - the answer the dirty-document guard gets (default: no). It is a RUN-time
                     setting: it is read when a `new`/`open` executes, and the LAST `discard`
                     executed before that `new`/`open` wins (a `discard` after it does not apply
                     to it; it applies to later switches).
                     any token count other than exactly 1 after `discard` (so `discard` and
                     `discard yes no`), parse error, exact:  'discard' expects 'yes|no'
                     exactly 1 token that is neither `yes` nor `no`, parse error, exact:
                                            expected 'yes' or 'no', got '<token as given>'
```
All three are ordinary script commands: on success they print the existing one-line echo
`[<line>] <tokens...> undo=<0|1> redo=<0|1> dirty=<0|1>` with the flags read AFTER the command.
- A successful `new` / `open` prints `undo=0 redo=0 dirty=0` - the new document has no history.
- **A cancelled switch (dirty and the answer was `no`) is NOT a failure**: the echo line is printed
  with the flags of the UNCHANGED document (so `dirty=1`, `undo=1` stay), exit code stays 0.
- A **failed** `open` is a run-time command failure exactly like the existing ones: exit **6**,
  stderr EXACTLY `dhunzilla: script line <n>: <loader error verbatim>`, stdout empty.
- Do NOT reorder the existing `enum class Cmd` members: the existing index-range checks rely on
  `expectGain..expectSolo` and `expectWidgetGain..expectWidgetSolo` staying contiguous. Append the new
  enumerators at the END.

### Component rebuild
`dz::MainComponent` builds its track rows and rack cells once per project, so after a SUCCESSFUL
`new`/`open` the component must be destroyed and rebuilt over the SAME `op::Document` object (never
re-allocate the `Document` itself - the component holds a reference to it). In `--script` mode keep the
component as `std::unique_ptr<dz::MainComponent>` re-created on a switch, so `expect widget ...`,
`click step ...` and a following `--snapshot` act on the NEW project. A cancelled or failed switch
does NOT rebuild anything.

### Settings file (Open Recent)
- Path: `<settings dir>/recent_projects.txt`, where the settings dir is `$DZ_APP_SETTINGS_DIR` when
  that environment variable is set and non-empty, else
  `juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("DhunZilla")`.
  Create the directory before the first write.
- Format: plain text, one absolute path per line, most recent first, at most **8** lines, each line
  ending with `\n`. No headers, no escaping. Read: a missing/unreadable file is the empty list; blank
  lines are skipped; only the first 8 non-blank lines are used.
- A successful open records `juce::File(<path as given>).getFullPathName()` (so a relative path is
  stored absolute): that path moves to the FRONT, any earlier copy is removed, the list is truncated
  to 8. Recording rewrites the whole file.
- A **failed** open does NOT touch the file, and `new` never records anything.
- A write failure (the directory cannot be created, the file cannot be opened, or the write/flush
  fails, e.g. `recent_projects.txt` is a directory) is exactly one stderr line
  `dhunzilla: cannot write recent projects file <full path of recent_projects.txt>` and changes no exit
  code (the `open` itself still succeeded: exit 0 if the script is otherwise fine). Flush the stream
  and test it for failure BEFORE it is destroyed.
- A cancelled switch (guard answered `no`) records nothing and leaves the settings file as it was.

### Window mode only (menus, chooser, discard box)
- File menu, in this order: `New`, `Open...` (ASCII dots, NOT U+2026), submenu `Open Recent`,
  separator, then the existing `Save` item (unchanged). The submenu lists the recent paths
  most-recent-first (at most 8, item ids `recentBase + index`, `recentBase = 100`); an empty list
  gives an empty submenu. The window title and the `--snapshot` `title:` line follow the CURRENT
  document path: `dz::windowTitleFor(doc.path())` = `DhunZilla \u2014 <file name of the path>` (an em dash U+2014 with one space each side, as `windowTitleFor` already produces), or plain `DhunZilla` when the document is untitled.
- `Open...` uses `juce::FileChooser` with title EXACTLY `Open Project` and wildcard EXACTLY
  `*.opj;*.opd`. Opening an `.opd` is not implemented: it fails through the loader (see Out of scope).
- **JUCE 9 has no modal loops in this build: `FileChooser::browseForFileToOpen`,
  `AlertWindow::showOkCancelBox` and any other blocking dialog API will NOT compile.** Use
  `chooser->launchAsync(...)` (keep the `FileChooser` alive in an `App` member until the callback
  runs) and `juce::AlertWindow::showAsync(juce::MessageBoxOptions()...)` with a
  `std::function<void (int)>` callback. An empty chooser result means cancelled: change nothing.
- The discard box (window mode, only when the document is dirty): title `Unsaved changes`, message
  `Discard the unsaved changes in the current project?`, buttons `Discard` (result 1) and `Keep`
  (result 0); `Keep` aborts the switch entirely. In `--script` mode there is NO box: the answer is
  the `discard yes|no` setting.
- A failed open in window mode prints the loader error verbatim as one `dhunzilla: <error>` stderr
  line and keeps the window and its current document open (no crash, no exit).

### Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, libm only via `dsp.hpp`, no fast-math; passes `-Werror` (the
app sources already carry `-Werror`) and the sanitizer build. Never mutate the model directly: the
document switch is `Document::open` / `Document::reset_to`, and every edit stays an operation through
`op::Document` (one undo entry per gesture).

## Acceptance criteria
- [x] Everything above implemented; existing script grammar, messages and exit codes unchanged.
- [x] Headless evidence (run these, paste the output in the handoff). With `DZ_APP_SETTINGS_DIR`
      pointing at an empty scratch dir, `dhunzilla --script <file> <a.opj>` where `a.opj` has 1 track
      and `b.opj` has 2 tracks:
  1. `open <b.opj>` + `expect gain 1 0.0` + `expect undo 0` + `expect redo 0` + `expect dirty 0`
     → exit 0 and line 1 of stdout is exactly `[1] open <b.opj> undo=0 redo=0 dirty=0`.
  2. `click mute 0` then `open <b.opj>` then `expect mute 0 1` → exit 0 (the switch was CANCELLED, the
     old document is intact); adding `discard yes` before the `open` instead gives exit 0 with
     `expect mute 0 0`, and the `open` echo line then reads `undo=0 redo=0 dirty=0`.
  3. `new` with `--save-as out.opj` → exit 0, and `out.opj` is byte-identical to the file
     `opd new ref.opj` writes for the same directory.
  4. `open <corrupt.opj>` (valid JSON, invalid project) → exit 6, stdout empty, and stderr equals the
     loader's own message: run `dhunzilla <corrupt.opj>` alone (exit 3, `dhunzilla: <error>`) and the
     script line must end with that same `<error>`.
  5. Pre-seed `recent_projects.txt` with 10 path lines interleaved with blank lines, run one `open`
     of a new project → exactly 8 lines, no blank line, the new path first, followed by the first 7
     non-blank seeded lines. Also: a `click mute 0` + `open` with no `discard yes` leaves a pre-seeded
     file byte-identical; and with the settings dir under a regular file (or `recent_projects.txt`
     being a directory) an `open` exits 0 with the exact write-failure stderr line above.
  5b. 10 opens of 10 different projects then a re-open of one already listed → `recent_projects.txt`
     holds exactly 8 lines, most recent first, duplicates removed, every path absolute (a relative
     `open p7.opj` is stored as its absolute path).
  6. After only `new` (no open) the settings file is absent or unchanged.
  7. `open <b.opj>` then `expect widget mute 1 0` and `expect widget grid 1 ................` then
     `click step 1 3` + `expect widget grid 1 ...x............` → exit 0 (proves the rebuild; without
     it the track/pattern index is out of range and the run exits 6).
  8. `open <b.opj>` with `--snapshot s.png` → exit 0, PNG 1100x700, and the `title:` line names
     `b.opj` (the CURRENT document), not the project file on the command line.
- [x] Existing behaviour unchanged, proven by an untouched legacy script:
      `click step 0 3` + `expect grid 0 ...x............` + `drag gain 0 -3.0` + `expect widget gain 0 -3.000`
      + `undo` → exit 0; and `bogus` → exit 4 with `script line 1: unknown command 'bogus'`.
- [x] Screenshots for the owner (non-blocking): a `--snapshot` PNG of a fixture project taken after a
      scripted `open`, attached to the handoff.
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md`, `docs/`, `harness/`, `.github/`, `tests/`, `src/engine/`, `src/cli/`,
  `src/app/main_component.*`, `CMakeLists.txt`, golden fixtures (`tests/fixtures/`).
- Loading `.opd` bundles (the chooser offers them; the loader error is what the user sees), Save As,
  a per-project settings store, autosave/recovery changes, keyboard shortcuts for New/Open, project
  templates, drag-and-drop, multiple documents or tabs, editing the recent list in the UI, pruning
  recent entries that no longer exist.
- Do not add tests to `tests/` or edit `tests/test_main.cpp`; headless coverage is the `--script`
  evidence above.

## Git
- Branch `wp-0212-app-open-new-project` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0212: app open/new - File menu New, Open, Open Recent and scriptable document switches`. Stage explicit paths only (never `git add -A`).
  Include `TASKS/WP-0212-app-open-new-project.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent

### What changed (3 files + this task file; nothing else touched)

- `src/doc/document.hpp` / `src/doc/document.cpp` — additive only: the ONE new public member
  `op::Document::reset_to(const Project& p, const std::string& path)`; it sets exactly
  `project_ = p`, `path_ = path`, `revision_ = 0`, `history_ = History()`,
  `saved_bytes_ = project_bytes(project_)`, `gesture_active_ = false`, `have_autosaved_ = false`.
  `open()/save()/save_as()` and every other member are untouched.
- `src/app/main.cpp` —
  - Script grammar: `new`, `open <path>`, `discard yes|no` appended at the END of `enum class Cmd`
    (`newDoc, openDoc, discardAnswer` — the expectGain..expectSolo / expectWidgetGain..expectWidgetSolo
    contiguity relied on by the index checks is preserved). Exact parse-error strings per spec.
    `discard` is a run-time `bool discard_answer` (default `no`) read when a `new`/`open` executes.
  - `App::switch_document(bool, const std::string&, bool, std::string&)` — the ONE document switch,
    fixed order: dirty guard (return 1, touches NOTHING) → `playback_->stop()` when a controller
    exists → `reset_to(op::new_project(op::NewProjectSpec{}), "")` or `op::Document::open`
    (failure → return 2, loader error verbatim) → rebuild component over the SAME Document →
    record the opened path (`new` never records). Return 0 on success.
  - Component rebuild: `--script` mode holds the component as member
    `std::unique_ptr<dz::MainComponent> script_component_`, destroyed and re-created on a switch;
    window mode uses `MainWindow::rebuildContent()`. `expect widget …` / `click step …` / a
    following `--snapshot` act on the NEW project; the Document object itself is never re-allocated.
  - `RecentProjects` class: `<settings dir>/recent_projects.txt` (settings dir =
    `$DZ_APP_SETTINGS_DIR` when set and non-empty, else `userApplicationDataDirectory/DhunZilla`);
    read skips blanks, keeps first 8; record moves the absolute path to front, dedupes, truncates
    to 8, rewrites the whole file (dir created first); write failure = exactly one stderr line
    `dhunzilla: cannot write recent projects file <full path>`, exit code unchanged. The
    flush-then-test-before-destroy pattern uses `std::ofstream` like `Document::autosave`
    (JUCE 9's `OutputStream::flush()` returns void).
  - Window mode: File menu order `New`, `Open...` (ASCII dots), `Open Recent` submenu (ids
    `recentBase + index`, `recentBase = 100`, most-recent-first, empty list → empty submenu),
    separator, existing `Save`. `Open...` = async `juce::FileChooser` (title `Open Project`,
    wildcard `*.opj;*.opd`) kept alive in `App::chooser_` until the callback; empty result =
    cancelled. Dirty document → `juce::AlertWindow::showAsync(MessageBoxOptions()…)` with
    `Discard`=1 / `Keep`=0 (no modal loops used anywhere). Window title and the `--snapshot`
    `title:` line follow `dz::windowTitleFor(doc.path())` (plain `DhunZilla` when untitled).

### How verified (real output)

Builds (dz_build.sh, `-j4`, `-Werror` on our sources, JUCE 9.0.5):
```
$ dz_build.sh openpattern --app --out build-wp0212                    → exit 0
$ grep -ci warning /home/shubhankar_dgx/qwen-scratch/build-wp0212/dz_build.log
0
```

Headless evidence driver: `/home/shubhankar_dgx/qwen-scratch/wp0212-evidence.sh`
(scratch `/home/shubhankar_dgx/qwen-scratch/wp0212-scratch`; `DZ_APP_SETTINGS_DIR` = fresh dir per run;
`a.opj` = `opd new a.opj` → 1 track; `b.opj` = `opd new b.opj --track Synth:synth --track Lead:synth` → 2 tracks;
`corrupt.opj` = `{"format_version": 0}`). Paths below shortened to `…/wp0212-scratch/`.

AC1 — `open b.opj` + expects → exit 0, exact first stdout line:
```
exit=0
[1] open …/b.opj undo=0 redo=0 dirty=0
[2] expect gain 1 0.0 undo=0 redo=0 dirty=0
[3] expect undo 0 undo=0 redo=0 dirty=0
[4] expect redo 0 undo=0 redo=0 dirty=0
[5] expect dirty 0 undo=0 redo=0 dirty=0
stderr: (empty)
line1 == '[1] open …/b.opj undo=0 redo=0 dirty=0'? 1
recent_projects.txt: …/b.opj
```

AC2 — cancelled vs discarded switch:
```
(click mute 0 / open b.opj / expect mute 0 1)  exit=0    ← CANCELLED, old document intact
[1] click mute 0 undo=1 redo=0 dirty=1
[2] open …/b.opj undo=1 redo=0 dirty=1
[3] expect mute 0 1 undo=1 redo=0 dirty=1
(click mute 0 / discard yes / open b.opj / expect mute 0 0)  exit=0
[1] click mute 0 undo=1 redo=0 dirty=1
[2] discard yes undo=1 redo=0 dirty=1
[3] open …/b.opj undo=0 redo=0 dirty=0
[4] expect mute 0 0 undo=0 redo=0 dirty=0
```

AC3 — `new` + `--save-as out.opj` == `opd new ref.opj`:
```
exit=0
[1] new undo=0 redo=0 dirty=0
saved: out.opj
cmp out.opj ref.opj: IDENTICAL
```

AC4 — failed open (`corrupt.opj`, valid JSON / invalid project):
```
open corrupt.opj → exit=6, stdout bytes: 0
stderr: dhunzilla: script line 1: tracks must be a non-empty array
dhunzilla corrupt.opj alone → exit=3
stderr: dhunzilla: tracks must be a non-empty array
MATCH: script stderr == 'dhunzilla: script line 1: ' + loader error verbatim
settings dir after failed open: (no recent_projects.txt)
```

AC5 — seeded `recent_projects.txt` (10 path lines + blank lines), one open of `c.opj`:
```
result file (8 lines): c.opj, seed1…seed7 (full absolute paths); no blank lines: 1
```
Cancelled switch leaves the pre-seeded file byte-identical: `recent_projects.txt UNCHANGED` (exit 0).
Write failures (settings dir under a regular file; `recent_projects.txt` is a directory):
```
exit=0; stderr: dhunzilla: cannot write recent projects file …/a_regular_file/subdir/recent_projects.txt
exit=0; stderr: dhunzilla: cannot write recent projects file …/settings5e/recent_projects.txt
```

AC5b — 10 opens, then a re-open of a listed project with the RELATIVE path `open p7.opj`
(cwd = the project dir) — exactly 8 lines, most recent first, deduped, all absolute (`all absolute: 1`):
```
…/rel/p7.opj   ← relative open stored absolute, moved to front
…/rel/p10.opj …/rel/p9.opj …/rel/p8.opj …/rel/p6.opj …/rel/p5.opj …/rel/p4.opj …/rel/p3.opj
```

AC6 — after only `new` (twice, with an edit between) the settings file stays absent:
`(no recent_projects.txt — absent, OK)`; the 2nd `new` on a dirty doc is cancelled, not a failure
(echo keeps `undo=1 redo=0 dirty=1`, exit 0).

AC7 — component rebuild proven (indices ≥1 exist only in b.opj): exit 0
```
[1] open …/b.opj undo=0 redo=0 dirty=0
[2] expect widget mute 1 0 undo=0 redo=0 dirty=0
[3] expect widget grid 1 ................ undo=0 redo=0 dirty=0
[4] click step 1 3 undo=1 redo=0 dirty=1
[5] expect widget grid 1 ...x............ undo=1 redo=0 dirty=1
```
Extra sanity (switch b.opj→default 1-track doc then drive it): `new` + `expect widget mute 0 0` +
`click step 0 5` + `expect widget grid 0 .....x..........` → exit 0.

AC8 — `open b.opj` with `--snapshot s.png`:
```
exit=0
title: DhunZilla — b.opj          ← CURRENT document, not a.opj on the command line
snapshot: …/s.png 1100x700
PNG size: 1100 x 700
```

Legacy unchanged (untouched scripts):
```
(click step 0 3 / expect grid 0 / drag gain 0 -3.0 / expect widget gain 0 -3.000 / undo)  exit=0
bogus → exit=4; stderr: dhunzilla: script line 1: unknown command 'bogus'
```
New parse errors (exact, exit 4): `'new' takes no arguments` · `'open' expects '<path>'` (0 and 2
tokens) · `'discard' expects 'yes|no'` (0 and 2 tokens) · `expected 'yes' or 'no', got 'maybe'`.

Gates (all against the WP-0212 build):
```
$ build-wp0212/op_tests                       → ALL PASS (0 failures)
$ harness/run_goldens.sh build-wp0212/opd     → negative-control (corrupt golden rejected)
                                               run_goldens: 16 fixtures, 0 failures
$ harness/test_cli_errors.sh build-wp0212/opd → test_cli_errors: 0 failures
$ build/opd symlink → harness/test_glue_scripts.sh (symlink removed afterwards)
                                              → test_glue_scripts: 0 failures
$ dz_build.sh --sanitize --target op_tests (DZ_BUILD_TYPE=Debug, as in the prior rig sanitizer
  builds build-san-wp0209/-wp0228)            → build OK; op_tests: ALL PASS (0 failures)
```

Demo artifact (owner, non-blocking): snapshot of the `tests/fixtures/goldens/demo.opj` fixture taken
after a scripted `open` → `/home/shubhankar_dgx/qwen-scratch/wp0212-scratch/owner_snapshot.png`
(1100x700, `title: DhunZilla — demo.opj`); AC8's b.opj snapshot is
`/home/shubhankar_dgx/qwen-scratch/wp0212-scratch/s.png`. Window-mode menus/chooser/discard box are
not screenshot-able headless (owner review in a real window).

### Metrics
- 3 source files changed: +312 / −28 lines (document.hpp +5, document.cpp +10, main.cpp +325/−28 —
  most of the main.cpp delta is the new grammar/switch/settings/menu code; the −28 is the mechanical
  `component.` → `script_component_->` conversion).
- Build log warnings: 0. Determinism: no clock/randomness added; settings file write is a plain
  file op outside engine paths; nothing engine-side changed (goldens bit-exact, 16/16).

### Notes / follow-ups
- PROPOSE (base-tree, out of scope here): the default-config sanitizer job (`-DDZ_SANITIZE=ON` with
  `CMAKE_BUILD_TYPE=Release`, g++ 13.3.0) fails to compile the BASE tree: a `-Werror=array-bounds=`
  false positive at `src/engine/render.cpp:786` (`out.arrangement = {clip};`, introduced by WP-0192).
  This WP touches no engine files; the sanitizer gate here passed with `DZ_BUILD_TYPE=Debug`, which
  is what the earlier sanitizer builds on this rig (build-san-wp0209, build-san-wp0228) used. Worth
  pinning the sanitizer job to Debug (or rewriting that assignment) so it does not depend on the
  build type.
- The `.opd` open path deliberately fails through the loader (per spec's Out of scope); no
  `.opd`-specific code was added.
