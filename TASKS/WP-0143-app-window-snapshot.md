---
id: WP-0143
title: JUCE app window + headless snapshot — transport bar and read-only track list (WP-0137b)
phase: 1
status: ready
depends_on: [WP-0141]
estimate_agent_hours: 2
human_review: screenshot-review
---

## Context — why this exists
WP-0137 (JUCE app skeleton) is split in four. WP-0141 (a) shipped the JUCE-free `op::Document`
(`src/doc/document.hpp`). This is part (b): the first pixels. A `dhunzilla` executable that shows a
main window — transport bar + track list, **read-only**, driven from `Document::track_rows()` — and a
headless `--snapshot` mode that renders that window to a PNG so it can be verified without a human.
Read first: `docs/adr/0007-gui-toolkit-and-audio-backend.md` (esp. §4 UTF-8 strings),
`spikes/juce-probe/CMakeLists.txt` and `spikes/juce-probe/Source/Main.cpp` (working JUCE 9.0.5 setup and
`createComponentSnapshot` usage — copy patterns, do not link or edit the spike), `src/doc/document.hpp`.

**Deferred (do NOT do here):** audio device / playback (part c), editing interactions + undo menu (part c),
CI job `app` in `.github/workflows/ci.yml` (part d). Play/Stop are inert; widgets are read-only.

## Spec — what "correct" means

### 1. CMake
In the top-level `CMakeLists.txt`, add (after the existing targets) `option(DZ_BUILD_APP "Build the JUCE app (dhunzilla)" OFF)`.
Everything below lives inside `if(DZ_BUILD_APP) ... endif()`; with the option OFF the configure must not
mention JUCE, must not create `_deps`, and builds exactly `opengine dzdoc opd op_tests` as today.
- FIRST `enable_language(C)` (top-level `project()` is CXX-only; JUCE aborts configure with "A C compiler is required" otherwise). Then `include(FetchContent)`; `FetchContent_Declare(JUCE URL https://github.com/juce-framework/JUCE/archive/refs/tags/9.0.5.tar.gz DOWNLOAD_EXTRACT_TIMESTAMP TRUE)`; `FetchContent_MakeAvailable(JUCE)` (same as the spike).
- `juce_add_gui_app(dhunzilla VERSION 0.1.0 PRODUCT_NAME "dhunzilla" COMPANY_NAME "DhunZilla")`(`VERSION` is mandatory: the top-level project has none and JUCE errors out without it); sources `src/app/main.cpp`
  and `src/app/main_component.cpp`; compile definitions `JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="dhunzilla" JUCE_APPLICATION_VERSION_STRING="0.1.0"`;
  `target_include_directories(dhunzilla PRIVATE src/app)`;
  `target_link_libraries(dhunzilla PRIVATE dzdoc juce::juce_gui_basics PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)`.
  (Link `juce_gui_basics` only — no audio modules in this WP.)
- Do NOT add `dhunzilla` to `DZ_OUR_TARGETS` (JUCE module sources compile inside that target; upstream
  warnings must not break the build). Instead, when `DZ_WERROR` is ON, apply `-Werror` to OUR two files only:
  `set_source_files_properties(src/app/main.cpp src/app/main_component.cpp PROPERTIES COMPILE_OPTIONS -Werror)`.
  Upstream JUCE warnings (e.g. `-Wdangling-reference` in `juce_LookAndFeel_V2.cpp`) are not ours — ignore them.
- The binary is produced under the build dir by JUCE (`<build>/dhunzilla_artefacts/<Config>/dhunzilla`);
  you do not need to move it. Use `find <build> -name dhunzilla -type f -perm -u+x` to locate it.

### 2. Local build recipe (no network) — put this in a comment above the option and in your handoff
JUCE 9.0.5 sources already exist on this machine. Build with:
```
export DISPLAY=${DISPLAY:-:1}; [ -n "${XAUTHORITY:-}" ] || export XAUTHORITY=/run/user/1000/gdm/Xauthority
JUCE_SRC=<workspace>/exp-juce/build/_deps/juce-src
cmake -S . -B build-app -DCMAKE_BUILD_TYPE=Release -DDZ_BUILD_APP=ON -DFETCHCONTENT_SOURCE_DIR_JUCE=$JUCE_SRC
cmake --build build-app -j4 --target dhunzilla       # cold JUCE build ≈ 3-5 min, ~several GB RAM: -j4 max
```
(If `$JUCE_SRC/modules` is missing, drop the `-DFETCHCONTENT_SOURCE_DIR_JUCE` flag and let FetchContent download.)
Never commit `build-app/`. Display: JUCE needs an X connection even for an offscreen snapshot (without a
`DISPLAY` the process segfaults at start-up). There is NO xvfb here; the desktop X server is `DISPLAY=:1`
with `XAUTHORITY=/run/user/1000/gdm/Xauthority`. Snapshot mode never shows a window, so it does not flash
the owner's screen — and it must never create a visible window (do not call `addToDesktop`/`setVisible` on top-level windows in snapshot mode).

### 3. `src/app/main_component.hpp` / `main_component.cpp` — exact API (namespace `dz`)
```cpp
namespace dz {
juce::String windowTitleFor(const std::string& project_path);  // "DhunZilla — " + file NAME of the path (U+2014 via CharPointer_UTF8)

class MainComponent final : public juce::Component {
public:
    static constexpr int kWidth = 1100;
    static constexpr int kHeight = 700;
    explicit MainComponent(const op::Document& doc);   // calls setSize(kWidth, kHeight); doc outlives the component
    void refresh();                                    // re-read doc.project()/doc.track_rows(); repaint
    void paint(juce::Graphics&) override;
    void resized() override;
    // plus whatever private members you need
};
}
```
Contents (readable defaults, dark background; polish is a later package):
- **Transport bar** (top 56 px): `Play` and `Stop` `juce::TextButton`s (disabled/inert), a BPM label showing
  `BPM <project().bpm with 1 decimal>` (read-only), a position label showing exactly `1.1`.
- **Track list** (below, one row of ~48 px per track, in order): name label; gain `juce::Slider` (LinearHorizontal,
  range −96..24, value = `gain_db`, with a text box); pan `juce::Slider` (Rotary, range −1..1, value = `pan`, no text box);
  `M` and `S` `juce::TextButton`s whose toggle state AND colour differ visibly when `mute` / `solo` is true
  (e.g. mute on = red-ish, solo on = amber); a row whose `audible` is false is drawn dimmed (e.g. name alpha 0.45).
- Everything is **read-only**: `setInterceptsMouseClicks(false, false)` on every row widget; set widget values
  with `juce::dontSendNotification`. All text literals go through `juce::CharPointer_UTF8` / `String::fromUTF8`.
- **Landmines (verified):** (1) call `setSize(kWidth, kHeight)` as the LAST statement of the constructor, after the row widgets exist — an early `setSize` runs `resized()` with no rows and the track list is never laid out (blank window). (2) A toggled `TextButton` is drawn with `buttonOnColourId`, not `buttonColourId`: set both (off = grey, on = red for M / amber for S). (3) Call `gain.setNumDecimalPlacesToDisplay(1)` so the text box reads `-12.0`.
- Row widgets are created in the constructor from `doc.track_rows().size()`; `refresh()` only updates values.

### 4. `src/app/main.cpp` — `juce::JUCEApplication` subclass named `App`, `START_JUCE_APPLICATION(App)`
Parse `getCommandLineParameterArray()` inside `initialise()`. Grammar (options and the project path may appear in any order):
`dhunzilla [project.opj]` → window mode; `dhunzilla --snapshot out.png [--apply ops.opjl] project.opj` → snapshot mode.
- **Window mode:** open `op::Document` (if a path was given) and show a `juce::DocumentWindow` (native title bar,
  title `windowTitleFor(path)`, or plain `DhunZilla` when no path) whose content is a `MainComponent`; close button quits.
  No audio device is opened. (Not exercised by the hidden check — do not run it from your tests; it opens a real window.)
- **Snapshot mode** (never opens any audio device, never makes a window visible):
  1. `op::Document::open(project)`; 2. if `--apply`: read the file, for each line skipping blank lines and lines whose
  first non-blank char is `#`, parse as a JSON object, collect all ops, then `Document::apply` them in order;
  3. `MainComponent comp(doc)`; `comp.createComponentSnapshot(comp.getLocalBounds(), true, 1.0f)` (scale **1.0** so the
  image is exactly 1100×700 whatever the display DPI); 4. write PNG with `juce::PNGImageFormat` to the `--snapshot` path
  (delete an existing file first); 5. print to **stdout** exactly two lines then exit 0:
  ```
  title: DhunZilla — <file name of project>
  snapshot: <out path as given> 1100x700
  ```
  (UTF-8; `<file name>` is the last path component, e.g. `demo.opj`). Finish by `setApplicationReturnValue(code); quit();`
  from `initialise()` (so the process exit code is `code`).
- **Exit codes** (all errors: one line `dhunzilla: <reason>` on stderr, nothing on stdout, no PNG written unless exit 0):
  | code | meaning |
  |---|---|
  | 0 | success |
  | 2 | usage: unknown `--option`; `--snapshot` / `--apply` without a value; `--apply` without `--snapshot`; `--snapshot` without a project; more than one project path |
  | 3 | project file missing/unreadable/invalid (`Document::open` false), or `--apply` journal file missing/unreadable |
  | 4 | journal content rejected: a line is not a JSON object / not valid JSON, or `Document::apply` returned false |
  | 5 | PNG could not be written (e.g. the output directory does not exist) |
  Exit-code 2 for usage errors must be produced without needing the file system (check arguments before opening anything).

## Acceptance criteria
- [ ] With the local recipe above: configure + `cmake --build build-app -j4 --target dhunzilla` succeed; zero
      warnings from `src/app/*` files (JUCE-internal warnings are OK).
- [ ] Default build (no `-DDZ_BUILD_APP`): `cmake -B build` output never mentions JUCE, no `build/_deps`, builds
      `opd` and `op_tests` as before. `grep -ril juce src/engine src/cli src/doc` → nothing.
- [ ] `<app> --snapshot /tmp/a.png tests/fixtures/goldens/demo.opj` → exit 0; PNG is exactly 1100×700 (check
      the IHDR with stdlib Python: width/height are big-endian uint32 at byte offsets 16 and 20); stdout is the two lines above.
      Look at the PNG with the Read tool: transport bar visible, 3 track rows (Metronome, Bassline, Kick).
- [ ] With `--apply` of a journal containing `{"op":"set_track_mute","index":0,"mute":true}` the PNG differs
      from the plain one and shows track 0's `M` button lit. Same for `{"op":"set_track_gain","index":1,"gain_db":-30}` (slider moved).
- [ ] Every exit code in the table verified by hand once (commands + codes in the handoff).
- [ ] Standard gates on the default build: `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`; sanitizer build
      (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`, default option OFF) `op_tests` pass.
- [ ] No tests for the GUI are added to `op_tests` (it must stay JUCE-free).

## Out of scope — do NOT touch
- Audio device, playback, transport logic, editing interactions, menus, undo/redo UI (part c).
- `.github/workflows/` and any CI change (part d). `ROADMAP.md` (the elephant adds the row at ship time).
- `src/engine/`, `src/cli/`, `src/doc/`, `docs/spec/`, `docs/adr/`, `spikes/`, golden fixtures, `ai-sidecar/`.
- Do not commit build directories or generated PNGs.

## Git
- Branch `wp-0143-app-window-snapshot` from `main`. Commit: `WP-0143: JUCE app window + headless snapshot`.
  Stage explicit paths only (`CMakeLists.txt`, `src/app/*`, this task file); never `git add -A` / `git add .`.
- **Do not push or merge.** Build with `-j4` at most; wrap every JUCE build in
  `flock <workspace>/agent-build-kit/.calib.lock <command>` if that file exists.

---
## Handoff report — completed by the agent
- **What changed:**
  - `CMakeLists.txt`: appended `option(DZ_BUILD_APP … OFF)` block with the local no-network build recipe in a
    comment above it (as spec §2 requires). Inside the option: `enable_language(C)`, FetchContent JUCE 9.0.5
    tarball, `juce_add_gui_app(dhunzilla VERSION 0.1.0 PRODUCT_NAME "dhunzilla" COMPANY_NAME "DhunZilla")`,
    sources `src/app/main.cpp` + `src/app/main_component.cpp`, the four compile definitions,
    `PRIVATE src/app` include, links `dzdoc juce::juce_gui_basics` + PUBLIC recommended config/warning flags,
    and `-Werror` via `set_source_files_properties` on OUR two files only when `DZ_WERROR=ON`.
    `dhunzilla` is deliberately NOT in `DZ_OUR_TARGETS`.
  - `src/app/main_component.hpp` / `.cpp` (new): `dz::windowTitleFor` ("DhunZilla — " + file name, U+2014 via
    `CharPointer_UTF8`) and `dz::MainComponent` exactly per spec §3 — transport bar (disabled Play/Stop,
    `BPM 120.0`, position `1.1`) + one read-only row per `track_rows()` entry (name label; gain Slider
    LinearHorizontal −96..24 with text box at 1 dp; pan Slider Rotary −1..1 no text box; M/S toggling TextButtons
    with off=grey / on=red / on=amber `buttonColourId`+`buttonOnColourId`; all row widgets
    `setInterceptsMouseClicks(false,false)`; inaudible rows dimmed: name alpha 0.45 + a ~35% near-black wash
    painted in `paintOverChildren`). All strings via `fromUTF8`/`CharPointer_UTF8` (ADR-0007 §4). `setSize(kWidth,
    kHeight)` is the last ctor statement.
  - `src/app/main.cpp` (new): `App : juce::JUCEApplication`, `START_JUCE_APPLICATION(App)`. Order-independent CLI
    grammar per spec §4; window mode (DocumentWindow + MainComponent, close quits — not exercised here, per spec);
    snapshot mode stacks a `MainComponent` (no top-level window ever created), `createComponentSnapshot(bounds,
    true, 1.0f)`, deletes any existing file then writes via `PNGImageFormat`; journal read = skip blank/`#` lines,
    parse ALL lines first, then apply in order; exit codes 0/2/3/4/5 with one-line `dhunzilla: <reason>` on stderr.
  - No changes to engine/cli/doc/tests/spec — nothing outside the files above.
- **How verified:** (commands + actual output, incl. each exit code)
  1. **App build (local recipe; `-DFETCHCONTENT_SOURCE_DIR_JUCE=$JUCE_SRC`, no network), clean recompile of both
     app files after final edit** — `flock …/.calib.lock cmake --build build-app -j4 --target dhunzilla`:
     `build exit=0`; warnings mentioning src/app: none (`grep -iE "warning|error" build-warn.log | grep src/app` →
     empty). Upstream JUCE `-Wdangling-reference` warnings appear in the cold build (e.g. juce_LookAndFeel_V2.cpp)
     and do not fail it, exactly as the spec anticipates.
  2. **Default config gate** (`cmake -S . -B build`, no `-DDZ_BUILD_APP`): configure exit 0, `grep -ci juce` on the
     full configure output = **0**, `build/_deps` absent, incremental build produces exactly
     `opengine dzdoc opd op_tests` (no dhunzilla). Sanitizer configure (`-DCMAKE_BUILD_TYPE=Debug
     -DDZ_SANITIZE=ON`) likewise: exit 0, juce mentions 0, no `_deps`. `grep -ril juce src/engine src/cli src/doc`
     → no matches (exit 1).
  3. **Snapshot mode:** `$BIN --snapshot $T/snap-demo.png tests/fixtures/goldens/demo.opj` → exit 0; stdout exactly:
     ```
     title: DhunZilla — demo.opj
     snapshot: <workspace>/dz-test/snap-demo.png 1100x700
     ```
     stderr empty. Stdlib-Python IHDR check: `IHDR 1100 x 700 bitdepth 8 colortype 6`.
     *Visual check honesty note:* this harness has no vision-capable model (the Read tool rejects PNG input), so the
     "look at the PNG" step was done **mechanically** by decoding both PNGs with Python stdlib (zlib+struct, all
     five filter types) and asserting rendered regions: background `(20,21,26)` in empty corners; transport strip
     `(28,30,38)` at y=28; separator `(44,47,58)` at y=55; BPM-label ink 154 px and position-`1.1` ink 32 px; three
     name regions with distinct ink counts (150/119/51 = Metronome/Bassline/Kick); pan rotaries drawn on all rows
     (max-lum 143); gain thumbs at x=451/463/471 vs expected 450/462/470 for −12/−6/−2 dB; M/S faces unlit grey.
     Both PNGs are **presented to the human for screenshot review** (the task's `human_review: screenshot-review`).
  4. **Criterion 4, exact journals:**
     - `{"op":"set_track_mute","index":0,"mute":true}` → exit 0; PNG differs from plain by 13338 sampled px in the
       row band; M-button reddish pixels per row: base `[0,0,0]` → applied `[1232,0,0]`; row-0 background wash
       `(18,19,23)` vs `(20,21,26)` on audible rows.
     - `{"op":"set_track_gain","index":1,"gain_db":-30}` → exit 0; PNG differs by 233 sampled px (row 1 only);
       row-1 gain thumb x 463 → **417** (expected ≈412 for −30 dB; ~5px thumb-width offset), rows 0/2 thumbs
       unchanged `[451,471]`.
     - Combined journal (comment line + blank line + mute + `gain_db:-9.5`) also verified: exit 0, row-2 thumb at
       456 (expected ≈455).
  5. **Exit-code matrix** (each run once by hand; stdout/stderr shown verbatim):
     ```
     apply-ok          exit=0 (want 0) stdout=[title: DhunZilla — demo.opj|snapshot: …/snap-applied.png 1100x700|] stderr=[]
     unknown-flag      exit=2 (want 2) stderr=[dhunzilla: usage: unknown option --bogus]
     snapshot-no-value exit=2 (want 2) stderr=[dhunzilla: usage: --snapshot requires a path]
     apply-w/o-snap    exit=2 (want 2) stderr=[dhunzilla: usage: --apply requires --snapshot]
     two-positional    exit=2 (want 2) stderr=[dhunzilla: usage: more than one project file given]
     snap-no-project   exit=2 (want 2) stderr=[dhunzilla: usage: --snapshot requires a project file]
     missing-project   exit=3 (want 3) stderr=[dhunzilla: cannot open '/nonexistent-project.opj']
     missing-journal   exit=3 (want 3) stderr=[dhunzilla: cannot read journal file …/nope.jsonl]   (+ ls: snap-x.png absent ✓)
     bad-json-line     exit=4 (want 4) stderr=[dhunzilla: journal line is not valid JSON: {"op": set_track_mute]
     op-rejected       exit=4 (want 4) stderr=[dhunzilla: journal operation rejected: set_track_mute: 'index' out of range]
     unwritable-png    exit=5 (want 5) stderr=[dhunzilla: cannot write PNG to /nonexistent-dir-xyz/out.png]
     ```
     All usage errors are produced before any filesystem access; no PNG is left behind on failure paths.
  6. **Standard gates on the default build:** `./build/op_tests` → `ALL PASS (0 failures)` exit 0;
     `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures` exit 0;
     `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures` exit 0;
     `bash harness/test_glue_scripts.sh` (with `UV_CACHE_DIR` set) → `test_glue_scripts: 0 failures` exit 0.
     Sanitizer build `-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON` (DZ_BUILD_APP left OFF): configure + build exit 0,
     `./build-san/op_tests` → `ALL PASS (0 failures)` exit 0.
  7. **No GUI tests added to `op_tests`** — the test sources were not touched; JUCE stays out of the default build graph.
- **Follow-ups noticed (not done):**
  - Snapshot-mode visual polish: row wash also dims lit controls on inaudible rows (tuned alpha 0x59 so red M
    stays readable through it); part-(c) may want dimming to skip button faces.
  - Track list is fixed-height rows from y=68 with no scroll — fine for demo.opj's 3 tracks, needs scrolling when
    track count grows (later WP).
  - `--apply` currently only affects snapshot mode; window mode ignores it (spec grammar implies exactly that).
  - JUCE 9 removed `File::readTextFile(String&)`; we use `createInputStream()` + `readEntireStreamAsString()`.
    Worth remembering for the spike docs, but spikes/ is out of scope here.
- **PROPOSE / BLOCKED:** none.
