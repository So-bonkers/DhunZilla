---
id: WP-0195
title: App channel rack panel - a 16-step grid per pattern in the JUCE window; a cell click issues `set_step` through `op::Document`
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: owner-screenshots
touches: [app]
---

## Context - why this exists
The prototype goal is "arrange a 3-minute beat in the app". Beats are made on a **channel rack**: a row of 16
step buttons per instrument. The engine side exists (WP-0154): `op::step_grid(pattern, 16)` renders a pattern
as `x...x...x...x...` and the ops `set_step` / `clear_steps` edit it, undoably. This package adds the
**panel** to the JUCE app window (`src/app/`), below the track rows. It is purely a view + a click handler:
**no UI code mutates state directly** - a cell click applies the operation `set_step` through
`op::Document::apply`, and every cell is re-read from the model by `refresh()` (exactly like the gain/pan/M/S
widgets of WP-0152). Nothing outside `src/app/` changes.

Existing APIs this uses (read their headers, do not change them): `op::step_grid` in `src/engine/ops.hpp`;
`op::Document` (`apply`, `project()`, `track_rows()`) in `src/doc/document.hpp`; the `set_step` operation
(`docs/spec/operations.md` section "Step sequencer": arguments `pattern`, `step`, `on`). Read
`src/app/main_component.{hpp,cpp}` and the script section of `src/app/main.cpp` first - you extend them in
their existing style.

## Spec - normative

### Files (exactly these 3; stage no others, except this task file)
`src/app/main_component.hpp`, `src/app/main_component.cpp`, `src/app/main.cpp`. No new files, no CMake change
(`main_component.cpp` is already in the `dhunzilla` target). Every change is ADDITIVE: no existing line of
behaviour, no existing script message and no existing constant may change.

### MainComponent additions (`main_component.hpp`, namespace `dz`)
```cpp
static constexpr int kRackSteps = 16;      // step cells per rack row
static constexpr int kRackRowHeight = 32;  // rack row pitch
static constexpr int kRackCellPitch = 36;  // cell-to-cell distance; each cell is 32x24
// Invokes the cell's onClick() std::function directly (never triggerClick()): exactly one `set_step`
// through Document::apply. On a bad index returns false and sets err to EXACTLY:
//   "pattern index <p> out of range (0..<n-1>)"   (n = patternCount(); checked first)
//   "step index <s> out of range (0..15)"
bool scriptStep(int pattern, int step, std::string& err);
// The 16 cells' ACTUAL toggle states as a 16-char string of 'x' (lit) and '.' (unlit); "" for a bad index.
std::string gridText(int pattern) const;
int patternCount() const;   // number of rack rows = project().patterns.size() when the component was built
```
Add the matching private members: `struct RackRow;` (defined in the .cpp: a `juce::Label name` and
`juce::TextButton cells[kRackSteps]`), `std::vector<std::unique_ptr<RackRow>> rack_;`, `int rack_top_ = 0;`.
Update the header comment block with a short "Channel rack (WP-0195)" paragraph.

### Behaviour (`main_component.cpp`)
- **Rows:** one rack row per pattern, in `doc.project().patterns` order, built ONCE in the constructor (like
  `rows_`; patterns added later by an op get no row - out of scope). Row label text = the name of
  `track_rows()[pattern.track]` when `pattern.track` is a valid track index, else `"Pattern <i>"` (0-based `i`).
- **Cell state comes only from the model:** `refresh()` (in addition to what it does today) sets, for every
  rack row `p` and step `s`, `cells[s].setToggleState(op::step_grid(project.patterns[p], kRackSteps)[s] == 'x',
  juce::dontSendNotification)` and the label text. Do NOT call `setClickingTogglesState(true)` on a cell, and do
  not keep any step state of your own.
- **Click:** each cell's `onClick` reads the CURRENT model state of its step via `op::step_grid(...,
  kRackSteps)`, then calls `doc_.apply(nlohmann::json{{"op","set_step"},{"pattern",p},{"step",s},
  {"on",!currently_on}}, err)` (ints for `pattern`/`step`, a JSON boolean for `on`), then `refresh()`
  (also when `apply` failed - the model stays the truth). One click = one undo entry. Turning an off step on
  creates the default step note; turning an on step off removes every note on that step (that is what
  `set_step` already does; do not re-implement it). Off-grid notes are never touched (also `set_step`'s job).
- **Layout (in `resized()`), all in component coordinates:**
  `rack_top_ = kTransportHeight + 12 + <track row count> * kRowHeight + 28` (so 240 for three tracks).
  Rack row `p` occupies y = `rack_top_ + p * kRackRowHeight`. Its label bounds: `(16, y, 240, kRackRowHeight)`.
  Cell `s` bounds: `(16 + 256 + s * kRackCellPitch, y + 4, 32, 24)`. Rows that would extend past the window
  are simply not handled (no scrolling in this package).
- **Look:** the cell's `buttonOnColourId` is the new file-scope colour `kStepOn{0xffe8742a}` (orange).
  Unlit cells: `buttonColourId` = the existing `kButtonOff` for steps 0-3 and 8-11, and the new
  `kStepOffAlt{0xff2a2d38}` for steps 4-7 and 12-15 (beat groups). Cells have no text. In `paint()`, when there
  is at least one rack row, draw the caption `Channel rack` (13 px font, `kCaption`, left-justified) inside the
  rectangle `(16, rack_top_ - 20, 240, 18)`. Row dimming (`paintOverChildren`) is unchanged.
- All string literals go through `juce::String::fromUTF8` as elsewhere in the file.

### Script grammar additions (`main.cpp`, headless `--script`)
Three new commands (add them to the grammar comment at the top of `main.cpp`):
```
click step <pattern> <step>                       - scriptStep(): ONE set_step through the Document
expect grid <pattern> <16 chars of x/.>           - the MODEL: op::step_grid(project().patterns[pattern], 16) equals the string
expect widget grid <pattern> <16 chars of x/.>    - the WIDGETS: component.gridText(pattern) equals the string
```
- Indices are parsed with the existing `parseTrack` helper (whole-token integer, negatives allowed, range-checked
  at run time). A grid string must be exactly 16 characters, each `x` or `.` (add a small helper `parseGrid`).
- Syntax errors (whole file is parsed first; exit 4; message on stderr `script line <n>: <reason>` as today):
  `click step` with a token count other than 4 -> `'click step' expects '<pattern> <step>'`;
  non-integer pattern -> `expected a pattern index, got '<tok>'`; non-integer step ->
  `expected a step index, got '<tok>'`; `expect grid` / `expect widget grid` with a wrong token count or a grid
  string that is not 16 x/. characters -> `'expect grid' expects '<pattern> <16 characters x or .>'` /
  `'expect widget grid' expects '<pattern> <16 characters x or .>'`.
- Run-time: `click step` with a bad index -> exit 6, reason = the `scriptStep` err string. `expect grid` /
  `expect widget grid` with a pattern index outside `[0, patternCount())` -> exit 6, reason
  `pattern index <p> out of range (0..<n-1>)`. A grid that differs -> exit 7, reason
  `expected grid <want>, got <got>` (widget form: `expected widget grid <want>, got <got>`).
- Success echo line is the existing one: `[<line>] <tokens joined by single spaces> undo=<0|1> redo=<0|1> dirty=<0|1>`.
- Existing commands, messages and exit codes stay exactly as they are (`click mute|solo`, `expect widget gain|...`).
  The new `click step` / `expect grid` / `expect widget grid` branches must not change how
  `click <other>` or `expect widget <other>` lines parse or fail.
- `--snapshot` (with or without `--apply`, with or without `--script`) is unchanged except that the PNG now
  contains the rack; the two output lines and the 1100x700 size are unchanged. `--apply` journals are applied
  BEFORE the component is built, so the rack shows the journal's result.

Constraints graded by grep on the diff of `src/app/`: `main_component.cpp` must contain the text `"set_step"` and
the text `step_grid(`; no file under `src/app/` may contain `const_cast`; `main_component.cpp` must not
contain `"clear_steps"` (the panel only ever issues `set_step`).

### Building and trying it (the check does exactly this)
```
export DISPLAY=${DISPLAY:-:1}; [ -z "${XAUTHORITY:-}" ] && export XAUTHORITY=/run/user/1000/gdm/Xauthority
cmake -S . -B <build-dir> -DCMAKE_BUILD_TYPE=Release -DDZ_BUILD_APP=ON \
      -DFETCHCONTENT_SOURCE_DIR_JUCE=<workspace>/exp-juce/build/_deps/juce-src
cmake --build <build-dir> -j4 --target dhunzilla      # cold JUCE build ~5 min; use -j4 at most
<build-dir>/dhunzilla_artefacts/Release/dhunzilla --script s.txt --snapshot out.png tests/fixtures/goldens/demo.opj
```
Example script on `tests/fixtures/goldens/demo.opj` (pattern 0 is the Metronome: steps 0, 4, 8, 12):
```
expect grid 0 x...x...x...x...
click step 0 1
expect grid 0 xx..x...x...x...
expect widget grid 0 xx..x...x...x...
undo
expect grid 0 x...x...x...x...
```
Look at the PNG you produce (you can open it): three orange-lit rows under a "Channel rack" caption.

## Acceptance criteria
- [x] Everything above implemented in exactly the three files; the default (non-app) build, `op_tests`,
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`
      are unaffected and green; the app target builds with 0 warnings from our sources (`-Werror` already applies).
- [x] `dhunzilla --snapshot out.png --apply j.opjl p.opj` draws the rack so that, for every pattern row `p` and
      step `s` (with `L = 16 + 256 + 36*s`, `Y = rack_top + 32*p + 4`, `rack_top` as defined above), the PNG
      satisfies - on the 1100x700 snapshot, pixel (x, y) as (R, G, B):
      - the four interior points `(L+8, Y+6)`, `(L+24, Y+6)`, `(L+8, Y+17)`, `(L+24, Y+17)` are ALL lit
        (`R >= 200` and `B <= 100`) iff `op::step_grid(pattern p, 16)[s] == 'x'` after the journal; otherwise they
        are ALL unlit (`35 <= R <= 100` and `R - B <= 40`);
      - the three gap points `(L+34, Y+12)`, `(L+16, Y-2)`, `(L+16, Y+26)` are window background
        (`R <= 30`, `G <= 30`, `B <= 40`).
- [x] The example script above and the grammar of the "Script grammar additions" section behave exactly as
      specified, including exit codes 4 / 6 / 7 and stderr reasons; stdout is empty on every failure.
- [x] One cell click = one undo entry; undo/redo restore the grid; after `click step` the saved project
      (`--save-as`) is byte-identical to `opd apply` of the equivalent `set_step` journal.
- [x] You looked at your own snapshot PNG (rows aligned, caption readable, lit vs unlit clearly distinct).

## Out of scope - do NOT touch
- Anything outside the three files: `ROADMAP.md`, `src/engine/`, `src/doc/`, `src/cli/`, `CMakeLists.txt`,
  `.github/`, `docs/`, `tests/`, `ai-sidecar/`, golden fixtures.
- Per-step velocity/pitch editing, drag-painting, right-click, scrolling or window resize, a "clear row"
  button, per-pattern length (>16 steps), row creation for patterns added after startup, audio preview of a
  clicked step, keyboard navigation of cells.

## Git
- Branch `wp-0195-app-channel-rack-panel` from `main`. Commit: `WP-0195: app channel rack panel (set_step via Document)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:** exactly three files, all additive (no existing line of behaviour, script message or
  constant touched):
  - `src/app/main_component.hpp` — "Channel rack (WP-0195)" header paragraph; constants `kRackSteps=16`,
    `kRackRowHeight=32`, `kRackCellPitch=36`; public `scriptStep(pattern, step, err)` / `gridText(pattern)`
    / `patternCount()` with the exact contract comments; private `struct RackRow` fwd-decl,
    `std::vector<std::unique_ptr<RackRow>> rack_`, `int rack_top_ = 0`.
  - `src/app/main_component.cpp` — colours `kStepOn{0xffe8742a}` / `kStepOffAlt{0xff2a2d38}`; `RackRow`
    (Label + 16 TextButtons); constructor builds one rack row per pattern once, cells get the beat-group
    unlit colour (`(s % 8) < 4` → `kButtonOff`, else `kStepOffAlt`) and `buttonOnColourId = kStepOn`; each
    cell's `onClick` reads its step via `op::step_grid(..., kRackSteps)` then applies
    `{"op":"set_step","pattern":p,"step":s,"on":!currently_on}` through `doc_.apply` and always `refresh()`es.
    `refresh()` re-reads every cell (`setToggleState`, `dontSendNotification`) and the row label
    (owning track's name, else `"Pattern <i>"`). `resized()`: `rack_top_ = kTransportHeight + 12 + rows *
    kRowHeight + 28` (= 240 for three tracks), label `(16, y, 240, 32)`, cell `(16+256+s*36, y+4, 32, 24)`.
    `paint()` draws the "Channel rack" caption (13 px, `kCaption`) at `(16, rack_top_-20, 240, 18)` when a
    row exists. No `setClickingTogglesState`, no local step state, no `clear_steps`, no `const_cast`.
  - `src/app/main.cpp` — grammar comment + three commands: `click step <pattern> <step>`,
    `expect grid <pattern> <16 chars x/.>` (model via `op::step_grid`), `expect widget grid <pattern> …`
    (widgets via `gridText`); new `parseGrid` helper; indices parsed with the existing `parseTrack`;
    exact syntax messages (`'click step' expects '<pattern> <step>'`, `expected a pattern index, got '<tok>'`,
    `expected a step index, got '<tok>'`, `'expect grid'/'expect widget grid' expects '<pattern> <16 characters x or .>'`);
    run-time exits: bad click indices → 6 with the `scriptStep` err (pattern checked first), grid-expect
    pattern outside `[0, patternCount())` → 6 `pattern index <p> out of range (0..<n-1>)`, grid mismatch → 7
    `expected grid <want>, got <got>` / `expected widget grid <want>, got <got>`. Existing click/expect-widget
    parse paths and messages are byte-for-byte unchanged.

- **How verified:** (all commands run on this aarch64 rig with
  `export DISPLAY=${DISPLAY:-:1}; export XAUTHORITY=/run/user/1000/gdm/Xauthority`; `UV_CACHE_DIR` exported for uv)
  1. **Default build + suites unaffected and green** (branch tip): `cmake --build build -j8` → rc 0;
     `./build/op_tests` → `ALL PASS (0 failures)`; `bash harness/run_goldens.sh` →
     `run_goldens: 13 fixtures, 0 failures`; `bash harness/test_cli_errors.sh` →
     `test_cli_errors: 0 failures`; `bash harness/test_glue_scripts.sh` →
     `test_glue_scripts: 0 failures`. Sanitizers (build-san, ASAN+UBSAN): op_tests `ALL PASS (0 failures)`,
     cli errors `0 failures`, goldens `13 fixtures, 0 failures`. Sidecar: `uv run pytest -q` →
     `146 passed, 2 skipped`; `uv run ruff check .` → `All checks passed!` (no sidecar files touched).
  2. **App builds with 0 warnings:** forced clean recompile of all three app sources
     (`touch src/app/main.cpp src/app/main_component.cpp src/app/playback_controller.cpp &&
     cmake --build build-app -j4 --target dhunzilla`) → `rc=0`, `grep -ciE "warning|error" = 0`
     (log: ../wp0195-verify/app_build.log); `-Werror` applies to these sources via source-file property.
  3. **Grep constraints on the diff:** `main_component.cpp` contains `"set_step"` (line 216) and
     `step_grid(` (lines 213, 270, …); `grep -rn const_cast src/app/` → 0; `clear_steps` in
     `main_component.cpp` → 0.
  4. **Example script** (verbatim from the task) on `tests/fixtures/goldens/demo.opj` → exit 0:
     ```
     [1] expect grid 0 x...x...x...x... undo=0 redo=0 dirty=0
     [2] click step 0 1 undo=1 redo=0 dirty=1
     [3] expect grid 0 xx..x...x...x... undo=1 redo=0 dirty=1
     [4] expect widget grid 0 xx..x...x...x... undo=1 redo=0 dirty=1
     [5] undo undo=0 redo=1 dirty=0
     [6] expect grid 0 x...x...x...x... undo=0 redo=1 dirty=0
     exit=0
     ```
  5. **Grammar, exact.** Syntax (all exit 4, stdout empty, `dhunzilla: script line 1: <reason>`):
     `click step 0` / `click step 0 1 2` / `click step` → `'click step' expects '<pattern> <step>'`;
     `click step x 1` → `expected a pattern index, got 'x'`; `click step 0 y` → `expected a step index, got 'y'`;
     `expect grid 0` / `expect grid 0 x...` / bad char / 17 chars / 5 tokens → `'expect grid' expects '<pattern> <16 characters x or .>'`;
     `expect widget grid 0` / `expect widget grid` / bad grid → `'expect widget grid' expects '<pattern> <16 characters x or .>'`.
     Regressions unchanged: `click mute` → `'click' expects 'mute|solo <track>'`; `click foo 0` →
     `unknown click control 'foo'`; `expect widget gain 0` → old generic message; `expect widget foo 0 1` →
     `unknown widget control 'foo'`; `expect gridx …` → `unknown expect object 'gridx'`.
     Run-time (all exit 6, stdout empty): `click step 5 0` → `pattern index 5 out of range (0..2)`;
     `click step -1 0` / `click step 5 99` → pattern error first; `click step 0 16` →
     `step index 16 out of range (0..15)`; `click step 0 -3` → `step index -3 out of range (0..15)`;
     `expect grid 7 …` / `expect widget grid -1 …` → `pattern index <p> out of range (0..2)`.
     Expect misses (exit 7, stdout empty): `expected grid ................, got x...x...x...x...`;
     `expected widget grid xx..x...x...x..., got x...x...x...x...`.
  6. **One click = one undo entry; undo/redo restore:** script `click step 2 5 → expect grid 2
     x...xx..x...x... → undo → expect grid 2 x...x...x...x... → redo → expect (grid+widget) restored` → exit 0,
     flags move 1/0→0/1→1/0 exactly. Mixing old commands with new in one script (`drag gain`, `click mute`,
     `key ctrl+z/shift+z`, `expect widget mute`, then `click step 1 7`) → exit 0.
  7. **Byte-identical save:** `dhunzilla --script <3 clicks> --save-as app_saved.opj demo.opj` vs
     `opd apply demo.opj j.opjl -o opd_applied.opj` with the equivalent three `set_step` journal →
     `cmp` clean: `BYTE-IDENTICAL: app --save-as == opd apply journal`.
  8. **Pixel acceptance** — `dhunzilla --snapshot out.png --apply j.opjl tests/fixtures/goldens/demo.opj`
     with a mixed journal (`set_step 0/8 off`, `1/3 on`, `2/14 on`; asserts both directions and both unlit
     shades); independent oracle = `opd steps <applied> p --steps 16`:
     ```
     grid 0: x...x.......x...
     grid 1: x..x..x.x...x.x.
     grid 2: x...x...x...x.x.
     pixel check: 3 rows x 16 cells, 0 failures (rack_top=240)   # all 48 cells × 4 interior pts + 144 gap pts
     ```
     Snapshot lines and size unchanged: `title: DhunZilla — demo.opj` / `snapshot: …/out.png 1100x700`.
  9. **Looked at the snapshot:** this model has no image input, so I inspected it by full-resolution ASCII
     rendering of the PNG (colour-classified pixel maps). Confirmed on `../wp0195-verify/out.png` and
     `example.png`: "Channel rack" caption glyphs readable at y≈225–231; per-row track labels
     ("Metronome", "Bassline", "Kick") render left of the cells at x≈16; three cell rows aligned in columns
     starting at x=272, pitch 36; lit cells orange (232,116,42) vs unlit faces (58,62,74)/(42,45,56) clearly
     distinct; per-row lit pattern matches the post-journal grid exactly. Screenshots for the owner:
     `../wp0195-rack-journal.png` (journal applied) and `../wp0195-rack-example.png` (after the example
     script's undo — model truth restored). During this visual check I caught and fixed one real bug before
     commit: rack row labels were missing `addAndMakeVisible`.
- **Metrics:** +237/−19 lines across exactly 3 files; app binary rebuilt, all CI-equivalent suites green on
  branch tip.
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE (out of scope, not done): the `--script` grammar comment in
  `main.cpp` and `docs/spec/` do not yet document the three new commands in a spec file — this WP's spec is
  the task file itself; if headless rack driving becomes a testing contract, a short `docs/spec/app-script.md`
  section would be worthwhile. No other files touched.
