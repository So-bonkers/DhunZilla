---
id: WP-0152
title: App track editing — gain/pan/mute/solo through op::Document, Undo/Redo/Save, scripted-input mode (WP-0137c)
phase: 1
status: ready
depends_on: [WP-0141, WP-0143]
estimate_agent_hours: 2
human_review: none
---

## Context — why this exists
The shipped JUCE app (`src/app/`, WP-0143) shows a **read-only** track list. This is part (c) of the app
skeleton: make it **editable** through `op::Document` (`src/doc/document.hpp`, read it first — especially
`begin_gesture/update_gesture/end_gesture`, `apply`, `undo/redo`, `save`) and add a **scripted-input mode** so
the editing can be verified headlessly: a script file drives the real `MainComponent` the way a user would
(slider drag, button click, Ctrl+Z), then optionally writes a snapshot PNG and the edited `.opj`.
Read first: `TASKS/WP-0143-app-window-snapshot.md` (build recipe, landmines, existing CLI/exit codes),
`src/app/main.cpp`, `src/app/main_component.{hpp,cpp}`, `docs/adr/0007-gui-toolkit-and-audio-backend.md` §4 (UTF-8).

**Deferred (do NOT do here):** audio/playback, a "save changes?" prompt on close, text-box rename, add/remove
tracks, drag-and-drop, CI `app` job. Dirty marker in the title bar is optional (not checked).

## Spec — what "correct" means

### 1. `MainComponent` becomes editable (`src/app/main_component.{hpp,cpp}`)
The model is the truth: after ANY change (user edit, undo, redo, script), call `refresh()`, which re-reads
`doc.track_rows()` and sets widgets with `dontSendNotification`. Widgets never hold state of their own.
The constructor now takes a **mutable** document: `explicit MainComponent(op::Document& doc);` (member
`op::Document& doc_`). Remove the `setInterceptsMouseClicks(false, false)` calls on the row controls and
remove `setClickingTogglesState(true)` from M/S (the lit state comes only from `refresh()`).
Keep `refresh()`, `windowTitleFor`, `kWidth/kHeight` and all visuals unchanged.

Interactions (wire them in the constructor, row index captured by value):
- **Gain slider** (`-96..24`): `onDragStart` → `doc_.begin_gesture()`; `onValueChange` → if
  `doc_.gesture_active()` then `doc_.update_gesture({"op":"set_track_gain","index":i,"gain_db":v})` else (keyboard /
  text box / wheel without a gesture) `doc_.apply(same op)`; `onDragEnd` → `doc_.end_gesture(err)`. After each of the
  three, call `refresh()`. A drag therefore yields **exactly one undo entry**; a drag that ends where it started yields none.
- **Pan rotary** (`-1..1`): identical, with `{"op":"set_track_pan","index":i,"pan":v}`.
- **M / S buttons**: `onClick` → `doc_.apply({"op":"set_track_mute","index":i,"mute":!track_rows()[i].mute})` (solo:
  `set_track_solo`, key `solo`), then `refresh()`. One click = one undo entry. If `apply` fails, `refresh()` still runs.
- `refresh()` must not itself trigger any callback (it already uses `dontSendNotification`).
- After every `refresh()` call `if (onDocumentChanged) onDocumentChanged();` (public member
  `std::function<void()> onDocumentChanged;`, used by the window to refresh menu enablement).

Commands used by menus, shortcuts and scripts — public methods, exact names:
```cpp
bool doUndo();                      // doc_.undo(); refresh(); returns the Document result
bool doRedo();                      // doc_.redo(); refresh()
bool doSave(std::string& err);      // doc_.save(err); refresh()
bool handleKey(const juce::KeyPress& key);   // Ctrl+Z → doUndo, Ctrl+Shift+Z → doRedo, Ctrl+S → doSave; true if consumed
bool keyPressed(const juce::KeyPress& key) override;   // = handleKey(key)
// scripted driving — each runs the SAME handlers a user's mouse would (see §2); err set on false
bool scriptDrag(const std::string& what, int track, double value, std::string& err); // what: "gain" | "pan"
bool scriptClick(const std::string& what, int track, std::string& err);              // what: "mute" | "solo"
double widgetValue(const std::string& what, int track) const; // what: gain|pan → slider value; mute|solo → toggle state 1.0/0.0
int trackCount() const;
```
- `handleKey`: "Ctrl" is `juce::ModifierKeys::commandModifier` (== Ctrl on Linux). Compare the key code
  case-insensitively (a Shift+Z press may report `'Z'`): `lower(key.getKeyCode()) == 'z'` and the modifier flags
  (`key.getModifiers()` masked to command|shift|alt) exactly `command` (undo) or `command|shift` (redo); `'s'` with exactly `command` → save.
  A failed command (nothing to undo, save error) is still "consumed" (returns true).
- `setWantsKeyboardFocus(true)` in the constructor (so key presses reach the component; child sliders bubble unhandled keys up).
- `scriptDrag("gain", i, v, err)`: error `track index <i> out of range (0..<n-1>)` if `i<0||i>=n`; error
  `unknown control <what>` for a bad `what`. Otherwise with `s = gain slider` (or pan): clamp `v` to the slider range,
  `cur = s.getValue()`; call `s.onDragStart()`; then for `k=1..4` `s.setValue(cur + (v-cur)*k/4.0, juce::sendNotificationSync)`
  (the last step uses exactly `v`; each triggers `onValueChange` like a real drag; setValue with an unchanged value
  fires nothing, as in real use); then `s.onDragEnd()`; return true unless `doc_.end_gesture` failed.
- `scriptClick("mute", i, err)`: same index error; invoke the button's `onClick()` std::function directly (`row.mute.onClick();`) — do NOT use `triggerClick()`: it posts an asynchronous message that never runs in a headless script; `unknown control <what>` otherwise.

Window mode: `MainWindow` gets a `juce::MenuBarModel` (`window.setMenuBar`): menus **File** (`Save`, shortcut text
`Ctrl+S`) and **Edit** (`Undo` `Ctrl+Z`, `Redo` `Ctrl+Shift+Z`); `Undo`/`Redo` items are disabled when
`!doc.can_undo()` / `!doc.can_redo()`; items call `doUndo/doUndo/doSave` of the content component;
`onDocumentChanged` calls `menuItemsChanged()`. Not exercised by the hidden check — never open a window in your tests.
A failed Save in window mode: write one line `dhunzilla: <err>` to stderr.

### 2. Scripted-input mode (`src/app/main.cpp`)
New CLI (options and the single project path in any order; extends the WP-0143 grammar, nothing is removed):
```
dhunzilla --script actions.txt [--snapshot out.png] [--save-as out.opj] [--apply ops.opjl] project.opj
```
`--script`, `--snapshot`, `--save-as`, `--apply` each take one value. Script mode is headless like snapshot mode
(no window is ever shown, no audio). Order of work: open project → `--apply` journal (as in WP-0143) → build
`MainComponent` → parse the WHOLE script → execute commands in order → `--snapshot` PNG (if given) → `--save-as` (if given).
Existing modes keep working unchanged: `dhunzilla --snapshot out.png [--apply j] project.opj` (no `--script`) prints
exactly its two WP-0143 lines.

**Script grammar.** UTF-8 text, one command per line; lines that are blank or whose first non-blank char is `#` are
skipped; tokens are separated by spaces/tabs; numbers are parsed with `std::strtod` over the whole token (a token
that is not fully a number is a syntax error), track indices with `std::strtol` (whole token). Commands (exact):
| line | meaning |
|---|---|
| `drag gain <track> <db>` | `scriptDrag("gain", track, db)` |
| `drag pan <track> <pan>` | `scriptDrag("pan", track, pan)` |
| `click mute <track>` | `scriptClick("mute", track)` |
| `click solo <track>` | `scriptClick("solo", track)` |
| `undo` | `doUndo()`; **fails (exit 6) if it returned false** (nothing to undo) |
| `redo` | `doRedo()`; fails (exit 6) if false |
| `save` | `doSave()` — writes the project file given on the command line; fails (exit 6) if it returns false |
| `key <combo>` | `combo` ∈ `ctrl+z`, `ctrl+shift+z`, `ctrl+s` (only these three; anything else is a syntax error): builds the `juce::KeyPress` (`'z'`/`'s'`, modifiers `commandModifier` [+`shiftModifier`]) and calls `handleKey`; never fails at runtime |
| `expect undo <0\|1>` | `doc.can_undo()` must equal the value |
| `expect redo <0\|1>` | `doc.can_redo()` must equal the value |
| `expect dirty <0\|1>` | `doc.dirty()` must equal the value |
| `expect gain <track> <db>` / `expect pan <track> <v>` | `track_rows()[track]` value within `0.001` of the number (model) |
| `expect mute <track> <0\|1>` / `expect solo <track> <0\|1>` | model flag equals |
| `expect widget gain <track> <db>` / `expect widget pan <track> <v>` / `expect widget mute <track> <0\|1>` / `expect widget solo <track> <0\|1>` | `widgetValue(...)` (the actual widget) within `0.001` of the number — proves widgets mirror the model |
A command with the wrong number of tokens, an unknown command/subcommand, a non-numeric number, a bool other than
`0`/`1`, is a **syntax error** (exit 4) — detected for the whole file BEFORE any command runs.
`<track>` out of range at run time → exit 6 (message from `scriptDrag`/`scriptClick`; for `expect` also exit 6 with
`track index <i> out of range (0..<n-1>)`).

**stdout** (only on exit 0; on any failure stdout is EMPTY — buffer it): one line per executed command, in order:
```
[<file line number, 1-based>] <command tokens joined by single spaces> undo=<0|1> redo=<0|1> dirty=<0|1>
```
(the three flags are `can_undo()`, `can_redo()`, `dirty()` AFTER the command). Then, if `--snapshot`: the two WP-0143 lines
(`title: DhunZilla — <file name>` / `snapshot: <path as given> 1100x700`). Then, if `--save-as`: `saved: <path as given>`.
`--save-as` uses `Document::save_as`, so the file bytes are `op::save_project` of the final state.

**Exit codes** (failures: exactly one line `dhunzilla: <reason>` on stderr, stdout empty, **no PNG and no `--save-as` file written**):
| code | meaning |
|---|---|
| 0 | success |
| 2 | usage: unknown `--option`; an option without its value; `--apply` without `--snapshot` or `--script`; `--snapshot`/`--script`/`--save-as` without a project; `--save-as` without `--script`; more than one project path. Checked before touching the file system |
| 3 | project, `--apply` journal or `--script` file missing/unreadable/invalid |
| 4 | journal content rejected (WP-0143) **or script syntax error** (`dhunzilla: script line <n>: <reason>`) |
| 5 | PNG or `--save-as` file cannot be written |
| 6 | a script command failed at run time (`undo`/`redo` with nothing to do, index out of range, `save` failed, gesture refused): `dhunzilla: script line <n>: <reason>` |
| 7 | an `expect` did not hold: `dhunzilla: script line <n>: expected <what> <want>, got <got>` |
A script that is valid but empty/comment-only succeeds (exit 0, no command lines). The save-as PNG/`.opj` of an exit-0
run is written even if the script made no change.

## Acceptance criteria
- [ ] App builds with the WP-0143 local recipe (`-DDZ_BUILD_APP=ON`, `-j4`), zero warnings from `src/app/*`.
- [ ] On `tests/fixtures/goldens/demo.opj` (copy it to a temp dir first — never write into `tests/fixtures`):
      `drag gain 0 -20` + `--save-as` → the saved JSON has `tracks[0].gain_db == -20` (±1e-4), nothing else changed;
      `click mute 1` + `click solo 2` → `mute`/`solo` flags in the saved JSON; `drag pan 1 0.5` → pan 0.5;
      after `drag gain 0 -20`, ONE `undo` returns `expect undo 0` and the saved bytes equal an unedited `--save-as`;
      `redo` re-applies; `key ctrl+z` / `key ctrl+shift+z` behave like `undo`/`redo`; `save` + `key ctrl+s`
      rewrite the project file; snapshot after edits differs from the plain snapshot; every exit code in the table verified by hand.
- [ ] Existing WP-0143 behaviour unchanged (`--snapshot`, `--apply`, its exit codes and two stdout lines).
- [ ] Default build (no `-DDZ_BUILD_APP`) untouched; standard gates pass: `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`; sanitizer build (`-DCMAKE_BUILD_TYPE=Debug
      -DDZ_SANITIZE=ON`) `op_tests` pass.
- [ ] No GUI tests added to `op_tests` (it stays JUCE-free).

## Out of scope — do NOT touch
- `src/engine/`, `src/cli/`, `src/doc/`, `docs/spec/`, `docs/adr/`, `spikes/`, `ai-sidecar/`, `.github/workflows/`,
  `harness/`, golden fixtures, `CMakeLists.txt` (no new files are needed). `ROADMAP.md` (the elephant adds the row at ship time).
- Audio/playback, close-confirmation, new widgets beyond what is listed. Do not commit build directories or PNGs.

## Git
- Branch `wp-0152-app-track-editing` from `main`. Commit: `WP-0152: app track editing + scripted input`.
  Stage explicit paths only (`src/app/*`, this task file); never `git add -A` / `git add .`.
- **Do not push or merge.** Build with `-j4` at most; wrap every JUCE build in
  `flock <workspace>/agent-build-kit/.calib.lock <command>` if that file exists.

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/app/main_component.hpp` / `.cpp`: constructor now takes a mutable `op::Document&` (member
    `op::Document& doc_`). Removed all five `setInterceptsMouseClicks(false,false)` calls on row controls and
    `setClickingTogglesState(true)` from M/S; added `setWantsKeyboardFocus(true)`. Wired per row (index captured
    by value): gain slider `onDragStart → doc_.begin_gesture()`, `onValueChange → gesture_active() ?
    update_gesture : apply` of `{"op":"set_track_gain","index":i,"gain_db":v}`, `onDragEnd → end_gesture(err)` +
    refresh (JUCE order verified in 9.0.5 source: `ScopedDragNotification` fires `onDragStart` in `mouseDown`
    BEFORE the first value change; `Slider::setValue` skips notification on an unchanged value — so a drag yields
    exactly one undo entry and a no-move drag none); pan identical with `set_track_pan`; M/S `onClick → apply`
    of `set_track_mute`/`set_track_solo` toggling `!track_rows()[i].flag`, then refresh (refresh runs even when
    apply failed). `refresh()` keeps all `dontSendNotification` writes and now ends with
    `if (onDocumentChanged) onDocumentChanged();` (public `std::function<void()> onDocumentChanged`). New public
    methods exactly per spec: `doUndo/doRedo/doSave/handleKey/keyPressed/scriptDrag/scriptClick/widgetValue/trackCount`.
    `handleKey` masks modifiers to command|shift|alt and compares case-insensitively ('z'+command → undo,
    'z'+command|shift → redo, 's'+command → save; consumed even on failure). `scriptDrag` clamps to the slider
    range, calls `onDragStart`, four `setValue(cur+(v-cur)*k/4, sendNotificationSync)` (last exactly `v`),
    `onDragEnd`; end_gesture failures surface via members set by the drag-end lambda. `scriptClick` invokes
    `button->onClick()` directly (not `triggerClick()`). Visuals, `refresh()`, `windowTitleFor`, kWidth/kHeight unchanged.
  - `src/app/main.cpp`: extended CLI grammar — `--script <file>` plus `--save-as <path>`; options and the single
    project path in any order. Script mode headless: open → `--apply` journal (WP-0143) → build `MainComponent` →
    parse WHOLE script (syntax errors exit 4 before any command runs) → execute → snapshot PNG (if asked) →
    save-as via `Document::save_as`. stdout fully buffered — printed only on exit 0: one line per executed command
    `[<line>] <tokens> undo=<0|1> redo=<0|1> dirty=<0|1>` (flags after the command), then the two WP-0143 lines if
    `--snapshot`, then `saved: <path>`. Exit codes 0/2/3/4/5/6/7 per table (script failures prefixed
    `dhunzilla: script line <n>: `); on save-as failure after a successful snapshot the PNG is deleted so no
    output survives. Window mode: `MainWindow` is now its own `juce::MenuBarModel` (`setMenuBar(this)`), File
    (Save, shortcut text Ctrl+S) and Edit (Undo Ctrl+Z, Redo Ctrl+Shift+Z; enabled from `doc.can_undo()/can_redo()`),
    items call the content component's `doUndo/doRedo/doSave`; a failed menu Save writes one line `dhunzilla: <err>`
    to stderr; `content_->onDocumentChanged = [this]{ menuItemsChanged(); }` (cleared in dtor). Without `--script`,
    snapshot mode is byte-for-byte the WP-0143 path. No changes outside `src/app/*`.
- **How verified:** (commands + actual output, incl. each exit code) — `$BIN = build-app/dhunzilla_artefacts/Release/dhunzilla`,
  all scratch work in `<workspace>/dz-test/wp0152` (fixture copied there; `tests/fixtures` never written).
  1. **Build** (`flock …/.calib.lock cmake --build build-app -j4 --target dhunzilla`, `-DFETCHCONTENT_SOURCE_DIR_JUCE=$JUCE_SRC`):
     exit 0. Forced clean recompile of both files: `Building CXX object …main_component.cpp.o / main.cpp.o`, then
     `grep -iE "warning|error" | grep src/app` → nothing (both compiled with `-Werror`).
  2. **drag gain + save-as** (`--script s_gain.txt --save-as out_gain.opj demo.opj`, script = `drag gain 0 -20` + expects):
     exit 0; stdout `[1] drag gain 0 -20 undo=1 redo=0 dirty=1 … [6] expect dirty 1 …` + `saved: out_gain.opj`;
     JSON diff vs original exactly `[('.tracks[0].gain_db', -12.0, -20.0)]` — nothing else changed, gain −20 ±1e-4 ✓.
  3. **click mute/solo**: `click mute 1 … undo (→mute 0, widget 0) … redo (→mute 1) … click solo 2` all expects pass,
     exit 0; saved JSON `t1.mute=True t2.solo=True`; one click = one undo entry ✓.
  4. **drag pan 1 0.5**: exit 0, saved JSON `t1.pan= 0.5` ✓.
  5. **undo semantics**: script `drag gain 0 -20; undo; expect undo 0; expect dirty 0; expect gain 0 -12; expect widget gain 0 -12`
     → exit 0, and `cmp base.opj back.opj` (baseline = comment-only-script `--save-as`) → **IDENTICAL to unedited save-as**;
     `redo` re-applies (`t0.gain_db= -20.0`, `expect redo 0`) ✓.
  6. **keys**: `key ctrl+z` → gain back −12/dirty 0/redo 1; `key ctrl+shift+z` → gain −20/dirty 1 — identical to undo/redo ✓.
     `save` rewrote `proj_save.opj` (`t0.gain_db after save= -20.0`, dirty 0); `key ctrl+s` rewrote `proj_keys2.opj` (−30) ✓.
  7. **snapshots**: plain `--snapshot snap_plain.png demo.opj` → exactly the two WP-0143 lines, IHDR 1100x700;
     script+snapshot prints command lines then the same two lines; PNG sha256 differs after edits (`86fd40…` vs `a700e0…`) ✓.
  8. **exit-code matrix** (each run by hand; stdout/stderr shown):
     ```
     unknown-flag        exit=2 [dhunzilla: usage: unknown option --bogus]
     opt-no-value        exit=2 [dhunzilla: usage: --snapshot requires a path]
     apply-w-o-mode      exit=2 [dhunzilla: usage: --apply requires --snapshot or --script]
     saveas-w-o-script   exit=2 [dhunzilla: usage: --save-as requires --script]
     snap-no-project / script-no-project  exit=2 (analogous messages)
     two-positional      exit=2 [dhunzilla: usage: more than one project file given]
     missing-project     exit=3 [dhunzilla: cannot open '/nonexistent-project.opj']
     missing-journal     exit=3 [dhunzilla: cannot read journal file nope.jl]  (no PNG left ✓)
     missing-script      exit=3 [dhunzilla: cannot read script file nope.txt]
     journal-bad-json    exit=4 [dhunzilla: journal line is not valid JSON: {"op": set_track_mute]
     script-unknown-cmd  exit=4 [… script line 1: unknown command 'frobnicate']   (also arg-count, non-numeric,
                       exit=4 for bad bool '2', bad combo 'alt+z', bad drag control 'foo')
     png-unwritable      exit=5 [dhunzilla: cannot write PNG to /nonexistent-dir-xyz/out.png]  (plain + script mode)
     snap-ok-saveas-fail exit=5 stdout bytes: 0, stderr one line, comb.png removed, no .opj ✓
     undo-nothing        exit=6 [… script line 1: nothing to undo]   undo-twice exit=6 (line 3)
     drag-index-oor      exit=6 [… track index 5 out of range (0..2)]  click-index-oor exit=6 [… track index 9 …]
     expect-index-oor    exit=6 [… track index 7 out of range (0..2)]  save-fails(ro file) exit=6 [save failed: cannot write 'ro.opj']
     expect-num-fail     exit=7 [… expected gain -99.000, got -20.000] expect-bool-fail exit=7; no PNG/save-as left ✓
     ```
     Exit 6 vs 7 routing was a real bug caught here (all failures initially exited 7); fixed and re-verified above.
  9. **Edge cases**: drag ending where it started → `undo=0 dirty=0`, expects pass; clamp `drag gain 0 -200`→−96 and
     `drag pan 2 5`→1 (`expect widget gain 0 -96` ✓); `key ctrl+s` on an unwritable project still exit 0 ("never fails");
     comment/blank/tab-separated lines skipped with correct original line numbers in echo (`[2]`, `[5]`); empty script +
     snapshot → only the two WP-0143 lines, exit 0.
  10. **WP-0143 unchanged**: `--snapshot out.png demo.opj` → exactly `title: DhunZilla — demo.opj` / `snapshot: … 1100x700`;
      `--apply j.jl --snapshot` works and PNG differs from plain ✓. (Only the `--apply` usage *message* now says
      "requires --snapshot or --script" per the new grammar; exit code unchanged.)
  11. **Standard gates (default build)**: `cmake -S . -B build` configure mentions JUCE **0** times, no `build/_deps`;
      `./build/op_tests` → `ALL PASS (0 failures)`; `bash harness/run_goldens.sh` → `run_goldens: 9 fixtures, 0 failures`;
      `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`; `bash harness/test_glue_scripts.sh` →
      `test_glue_scripts: 0 failures` (see env note); sanitizer build (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`,
      app OFF): configure exit 0, JUCE mentions 0, `./build-san/op_tests` → `ALL PASS (0 failures)`.
      `grep -ril juce src/engine src/cli src/doc` → no matches. **No GUI tests added to op_tests** — nothing under
      `tests/`, `src/engine/`, `src/cli/`, `src/doc/` touched (`git status`: only `src/app/*` + this task file).
- **Follow-ups noticed (not done):**
  - Menu-bar accelerators: items carry shortcut *text* per spec; actual Ctrl+Z/Ctrl+S handling in window mode goes
    through `MainComponent::keyPressed`/`handleKey`. A hidden check never opens a window, so the native-menu accelerator
    path is compile-verified only.
  - The inaudible-row wash (`paintOverChildren`) still dims lit controls on muted rows (WP-0143 note; unchanged here).
  - `scriptDrag` clamps out-of-range values silently (spec); a future UX pass might surface the clamp in a status line.
- **PROPOSE / BLOCKED:** none blocked. Env note: the instructed `UV_CACHE_DIR=<home>/.cache/uv` is on a
  read-only filesystem for this sandbox (`Could not create temporary file … Read-only file system`), so glue tests were
  run with the writable equivalent `UV_CACHE_DIR=<workspace>/.cache/uv` (0 failures).
