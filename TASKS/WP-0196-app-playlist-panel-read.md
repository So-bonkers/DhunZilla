---
id: WP-0196
title: App playlist view (read-only) - arrangement clips as blocks on per-track lanes over a bar grid, playhead from the dzplayer sample counter
phase: 1
status: ready
depends_on: [WP-0195]
estimate_agent_hours: 1.0
human_review: owner-screenshots
touches: [app, cmake, new:src/app/playlist_view.hpp, new:src/app/playlist_view.cpp, new:tests/test_playlist_view.cpp]
---

## Context - why this exists
Phase 1's exit criterion is "arrange a beat in the playlist", so the app must first SHOW the
arrangement. This package adds a read-only playlist panel to the JUCE window, below the channel rack
of WP-0195: one lane per track, a 16-bar grid, one block per arrangement clip (start bar, repeats as
dividers, fades as wedges, trim shortening the block) and a playhead whose position comes from the
Player's sample counter. It changes no model field, no operation, no render and no golden: the panel
never applies an operation and never mutates the `op::Document`. Editing comes in a later WP.

The geometry is computed by a NEW JUCE-free module `dz::playlist_view` (added to the existing
`dzplayer` library), so the exact numbers the widget paints are unit-testable headless. Existing code
this package reuses (read it first, never change it, never re-derive its maths): `op::Clip`,
`op::Project::arrangement`, `op::pattern_extent` in `src/engine/model.hpp`; `op::sample_at_bar` in
`src/engine/render.hpp` (the renderer's own tempo map); `op::Document::track_rows()`/`project()` in
`src/doc/document.hpp`; `dz::Player::position_frames()` through `PlaybackController::player()`
(`src/app/player.hpp`, `src/app/playback_controller.hpp`); the layout/style of the channel rack in
`src/app/main_component.{hpp,cpp}` (WP-0195) - extend it in the same style. Clip trim fields
(`offset_beats`, `length_beats`, WP-0193a) and fades (WP-0178) already exist at the base.

## Spec - normative (this task file is the contract)

### Files (exactly these 6; stage no others)
- `src/app/playlist_view.hpp` (new) - constants, `dz::PlaylistBlock`, and the three functions below.
  Include guard `#pragma once`; its only includes are `<string>`, `<vector>` and `"engine/model.hpp"`.
  **No JUCE: the lowercase string `juce` (an include, `juce::`, a comment) may not appear in either of
  the two new module files.**
- `src/app/playlist_view.cpp` (new) - the implementation. Pure: no I/O, no clock, no state, no
  mutation of the `op::Project`.
- `src/app/main_component.hpp` - additive only: a private `void paintPlaylist(juce::Graphics&);`
  declaration, a private `int playlist_top_ = 0;` member, and a short header-comment paragraph
  "Playlist (WP-0196, read-only)". Change no existing line.
- `src/app/main_component.cpp` - additive only: `#include "playlist_view.hpp"`, four file-scope
  colours, `paintPlaylist` (below), one `paintPlaylist(g);` call, one `playlist_top_` line in
  `resized()`, one `repaint();` at the end of `timerCallback()`. Do not modify or delete any existing
  statement (the channel rack of WP-0195 must stay pixel-identical), and do NOT touch
  `paintOverChildren`.
- `tests/test_playlist_view.cpp` (new) - own `main`, own `CHECK(cond, name)` macro printing
  `PASS <name>` on success and `FAIL <name>  (<file>:<line>)` on failure, exit code = failure count.
  **Every check name starts with `playlist.`** (e.g. `playlist.x_half_bar`). Build the projects in
  code; do no file I/O. Do NOT edit `tests/test_main.cpp`.
- `CMakeLists.txt` - purely additive, exactly these four lines:
  - after `target_link_libraries(dzplayer PUBLIC opengine)`:
    `target_sources(dzplayer PRIVATE src/app/playlist_view.cpp)  # WP-0196: playlist geometry (JUCE-free)`
  - after `add_test(NAME tracks COMMAND op_tracks_tests)`:
    ```cmake
    # WP-0196: read-only playlist geometry (dz::playlist_blocks in the JUCE-free dzplayer core).
    add_executable(op_playlist_view_tests tests/test_playlist_view.cpp)
    target_link_libraries(op_playlist_view_tests PRIVATE opengine dzplayer)
    add_test(NAME playlist_view COMMAND op_playlist_view_tests)
    ```
  - after `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227`:
    `list(APPEND DZ_OUR_TARGETS op_playlist_view_tests)  # WP-0196`
  Do not touch the `if(DZ_BUILD_APP)` block: `playlist_view.cpp` reaches the app through `dzplayer`.

### Interface (`namespace dz`, in `src/app/playlist_view.hpp`)
```cpp
inline constexpr int kPlaylistLeft = 272;             // x of bar 0
inline constexpr int kPlaylistBars = 16;              // bars 0..15 are drawn, no scrolling
inline constexpr int kPlaylistBarWidth = 48;          // px per bar
inline constexpr int kPlaylistLaneHeight = 32;        // lane pitch
inline constexpr int kPlaylistLaneDrawnHeight = 31;   // lane background / bar-line height
inline constexpr int kPlaylistBlockTop = 3;           // block top inset inside its lane
inline constexpr int kPlaylistBlockHeight = 26;       // block height
inline constexpr int kPlaylistBlockMinWidth = 2;      // narrowest drawn block
inline constexpr int kPlaylistPlayheadWidth = 2;      // playhead line width
inline constexpr int kPlaylistRight = 272 + 16 * 48;  // 1040: first x NOT drawn (exclusive edge)

int playlist_x(double bar);
double bars_at_frame(const op::Project& p, long long frame);

struct PlaylistBlock {
    int clip = 0;                 // index into p.arrangement
    int lane = 0;                 // patterns[clip.pattern].track (may be out of range)
    bool drawn = false;           // false: invalid pattern index, invalid lane, or x0 >= kPlaylistRight
    int x0 = 0, x1 = 0;           // absolute px
    int repeats = 1;              // clip.repeats, but 1 when the pattern extent is 0.0
    double repeat_beats = 0.0;    // op::pattern_extent(pattern); 0.0 for an invalid pattern index
    double start_bar = 0.0;       // clip.start_bar
    double length_beats = 0.0;    // *clip.length_beats when trimmed, else repeats * repeat_beats;
                                  // 0.0 for an invalid pattern index
    bool trimmed = false;         // clip.length_beats.has_value()
    double fade_in_beats = 0.0;   // clip.fade_in_beats
    double fade_out_beats = 0.0;  // clip.fade_out_beats
    std::vector<int> dividers;    // absolute x of every repeat divider strictly inside (x0, x1)
    int fade_in_px = 0;           // wedge width; 0 = none, else 0 < fade_in_px <= x1 - x0
    int fade_out_px = 0;          // wedge width; 0 = none, else 0 < fade_out_px <= x1 - x0
    std::string label;            // pattern id, or "Pattern <clip.pattern>" when the id is empty
    int label_x = 0;              // x0 + 4
    int label_w = 0;              // max(x1 - x0 - 8, 1)
};
std::vector<PlaylistBlock> playlist_blocks(const op::Project& p);
```

`playlist_x(bar)` - bar position -> absolute x. NaN or `bar <= 0.0` -> `kPlaylistLeft`; a bar whose
scaled offset `bar * kPlaylistBarWidth` is >= `kPlaylistBars * kPlaylistBarWidth` -> `kPlaylistRight`;
otherwise `kPlaylistLeft + llround(bar * kPlaylistBarWidth)` (`std::llround`: half away from zero).
Examples: `playlist_x(0.0) == 272`, `playlist_x(0.5) == 296`, `playlist_x(1.0) == 320`,
`playlist_x(-3.0) == 272`, `playlist_x(0.15625) == 280` (7.5 -> 8), `playlist_x(16.0) == 1040`.

`bars_at_frame(p, frame)` - transport position in BARS for a `dz::Player` sample-counter value,
through the renderer's own tempo map `op::sample_at_bar`; never a constant-bpm computation.
- `frame <= 0` -> `0.0`; `p.length_bars <= 0.0` -> `0.0`.
- `op::sample_at_bar(p, p.length_bars) <= frame` -> `p.length_bars` (park at the end).
- otherwise: `lo = 0.0`, `hi = p.length_bars`, then exactly 60 iterations of
  `mid = 0.5 * (lo + hi); if (op::sample_at_bar(p, mid) >= frame) hi = mid; else lo = mid;` and return `hi`.
Required properties (tested): non-decreasing in `frame`;
`|op::sample_at_bar(p, bars_at_frame(p, frame)) - frame| <= 1` for `0 < frame < sample_at_bar(p, length_bars)`;
for `tests/fixtures/goldens/tempo.opj` (120 bpm, then 72 bpm from bar 4) `bars_at_frame(p, 544000)` is
`5.0` within `1e-4` (a constant-120-bpm computation would give about `5.667`, which is wrong).
Bisection and the engine's rounding mean the answer is never exact: compare with tolerance `1e-4` bars.

`playlist_blocks(p)` - one entry per `p.arrangement` clip, in **model order**: never sorted, never
merged, and the legacy empty arrangement (`p.arrangement` empty) yields an **empty vector** - do NOT
synthesize implicit placements. Let `bb = p.beats_per_bar >= 1 ? p.beats_per_bar : 1` (as a double);
every bar<->beat conversion here is the plain product/quotient with `bb` (exactly `op::beats_at_bar`),
NEVER the tempo map. `clip.offset_beats` does not affect the geometry at all. For each clip:
- `clip` = its index; `start_bar`, `trimmed`, `fade_in_beats`, `fade_out_beats` are copied as stored.
- `label` = the pattern's `id` when non-empty, else `"Pattern " + std::to_string(clip.pattern)` -
  reported even for a block that is not drawn.
- Pattern index invalid (`clip.pattern < 0` or `>= p.patterns.size()`): `lane` stays `0`,
  `repeat_beats` and `length_beats` are `0.0`, `repeats` stays `clip.repeats`, `drawn` is `false`.
- Otherwise `lane = patterns[clip.pattern].track`, `repeat_beats = op::pattern_extent(pattern)`;
  if `repeat_beats <= 0.0` then `repeats = 1`;
  `length_beats = trimmed ? *clip.length_beats : repeats * repeat_beats`.
- `x0 = playlist_x(start_bar)`; `end_bar = start_bar + length_beats / bb`;
  `x1 = min(max(playlist_x(end_bar), x0 + kPlaylistBlockMinWidth), kPlaylistRight)`.
- `drawn = pattern_ok && lane indexes p.tracks && x0 < kPlaylistRight`. So a clip starting at
  `start_bar >= 16.0` is not drawn at all, and one starting inside is clamped to `x1 == 1040`.
- `label_x = x0 + 4`; `label_w = max(x1 - x0 - 8, 1)` (computed for skipped blocks too).
- `dividers` (only when `drawn && repeat_beats > 0.0`): for `r = 1 .. repeats - 1`,
  `xr = playlist_x(start_bar + r * repeat_beats / bb)`, pushed in `r` order **only when
  `x0 < xr < x1`** - a divider exactly at `x0` or exactly at `x1` is NOT pushed.
- `fade_in_px` (only when `drawn && fade_in_beats > 0.0`):
  `max(0, min(playlist_x(start_bar + fade_in_beats / bb) - x0, x1 - x0))`; `fade_out_px` likewise
  with `max(0, min(x1 - playlist_x(end_bar - fade_out_beats / bb), x1 - x0))`. A fade longer than the
  block clamps to the block width; `fade_*_beats == 0.0` gives `0`.
Worked example, `tests/fixtures/goldens/clip_fades.opj` (4/4, one track, one pattern of extent 1.5
beats): clip 0 (`start_bar 0`, `repeats 2`, fades `0.5`/`1.0`) -> `x0 272`, `x1 308`, `dividers {290}`,
`fade_in_px 6`, `fade_out_px 12`, `label "Pattern 0"`, `label_x 276`, `label_w 28`; clip 1
(`start_bar 3`, `repeats 1`) -> `x0 416`, `x1 434`, no dividers, no fade px.

### Widget (`src/app/main_component.cpp`)
Four new file-scope colours in the existing colour block, with EXACTLY these values
(`juce::Colour`'s constructor is not constexpr, so plain `const Colour`, like the others):
`kLaneBg{0xff1a1c24}`, `kClipFill{0xff3d7ea6}`, `kClipRepeat{0xff2a5877}`, `kPlayhead{0xffffd23f}`.

`resized()`: as its LAST statement (after the channel-rack loop),
`playlist_top_ = rack_top_ + static_cast<int>(rack_.size()) * kRackRowHeight + 28;`
(`kRackRowHeight` is the existing WP-0195 constant, value 32). So `playlist_top_` (y of lane 0) is
`56 + 12 + 48 * tracks + 28 + 32 * patterns + 28`: **204** for `clip_fades.opj` (1 track, 1 pattern),
364 for `demo.opj` (3, 3), 396 for `arranged.opj` (3, 4), 284 for `tempo.opj` and `clip_trim.opj`
(2 tracks, 2 patterns).

`paint(g)`: `paintPlaylist(g);` is the LAST statement of `paint()`.

`timerCallback()`: ends with one new statement `repaint();` (the playhead is read inside
`paintPlaylist`, so each 30 Hz tick must repaint the panel). Store no position in the timer.

`paintPlaylist(juce::Graphics& g)`, in this order (all strings through `juce::String::fromUTF8`):
1. `const auto tracks = doc_.track_rows();` - return immediately when `tracks` is empty.
2. `const long long frame = playback_ != nullptr ? playback_->player().position_frames() : 0;` -
   the sample counter is the ONLY playhead source: no wall clock, no bar/beat parsing, no second
   member holding a position.
3. Caption: 13 px font, `kCaption`, text `Playlist`, `drawFittedText` into
   `(16, playlist_top_ - 20, 240, 18)`, centredLeft, 1 line.
4. For every lane `t` (index into `tracks`, in order): `y = playlist_top_ + t * kPlaylistLaneHeight`;
   the track name (`kText`, 13 px) into `(16, y, 240, kPlaylistLaneHeight)`; then the lane background
   `kLaneBg` filled at `(kPlaylistLeft, y, kPlaylistBars * kPlaylistBarWidth, kPlaylistLaneDrawnHeight)`
   (= `x 272..1039`, `height 31`); then for every `b` in `0..kPlaylistBars-1` a `kSeparator` bar line
   at `(kPlaylistLeft + b * kPlaylistBarWidth, y, 1, kPlaylistLaneDrawnHeight)`.
   Lanes are NOT dimmed for inaudible tracks - a muted or un-soloed track's lane is drawn exactly like
   any other, and `paintOverChildren()` is not changed.
5. For every block of `dz::playlist_blocks(doc_.project())`, in order (a later clip paints over an
   earlier one); skip a block whose `drawn` is false. With `top = playlist_top_ + block.lane *
   kPlaylistLaneHeight + kPlaylistBlockTop` and `height = kPlaylistBlockHeight` (26, so a lane-0 block
   of `clip_fades.opj` covers y `207..232`):
   - `kClipFill` fill `(block.x0, top, block.x1 - block.x0, height)`;
   - `kClipRepeat` fill `(xr, top, 1, height)` for every `xr` in `block.dividers`;
   - if `fade_in_px > 0`: `kLaneBg`, a closed `juce::Path` through
     `(x0, top) -> (x0 + fade_in_px, top) -> (x0, top + height)` and back (float corners), `g.fillPath`
     - full width at the TOP, narrowing to the bottom-left corner;
   - if `fade_out_px > 0`: `kLaneBg`, a closed `juce::Path` through
     `(x1, top) -> (x1 - fade_out_px, top) -> (x1, top + height)` and back, `g.fillPath`;
     (JUCE 9's `juce::Graphics` has **no** `fillTriangle` - the wedges must be `juce::Path`s.)
   - label: 11 px font, `kText`, `drawFittedText` into `(block.label_x, top, block.label_w, height)`.
6. Playhead LAST: `hx = dz::playlist_x(dz::bars_at_frame(project, frame))`; `kPlayhead` filled at
   `(hx, playlist_top_, kPlaylistPlayheadWidth, int(tracks.size()) * kPlaylistLaneHeight - 1)`
   (for 1 lane: y `204..234`; for 3 lanes: y `396..490`). At `frame 0` (snapshot mode) `hx == 272`.

The panel is read-only: an added line of `src/app/main_component.cpp` may not contain `apply(`,
`begin_gesture`, `update_gesture` or `end_gesture` (not even in a comment), and `const_cast`,
`std::chrono`, `steady_clock` and `system_clock` may not appear anywhere under `src/app`.

### Building, snapshotting and the owner screenshot set
```
export DISPLAY=${DISPLAY:-:1}; [ -z "${XAUTHORITY:-}" ] && export XAUTHORITY=/run/user/1000/gdm/Xauthority
cmake -S . -B <build-dir> -DCMAKE_BUILD_TYPE=Release -DDZ_BUILD_APP=ON \
      -DFETCHCONTENT_SOURCE_DIR_JUCE=<workspace>/exp-juce/build/_deps/juce-src
cmake --build <build-dir> -j4 --target dhunzilla          # cold JUCE build ~5 min, -j4 at most
<build-dir>/dhunzilla_artefacts/Release/dhunzilla --snapshot out.png tests/fixtures/goldens/clip_fades.opj
```
Look at your own PNGs before handing off, and name their paths in the Handoff: a `--snapshot` of
`tests/fixtures/goldens/clip_fades.opj` (fades + repeat divider), of `tests/fixtures/goldens/arranged.opj`
(3 lanes, 5 clips, two lanes with overlapping blocks) and of `tests/fixtures/goldens/clip_trim.opj`
(trimmed clips: the block ends early and its later dividers disappear). Lanes aligned, blocks on the
right bars, fades visible as wedges, the playhead at bar 0.

## Acceptance criteria
- [ ] The 6 files above and nothing else; `src/app/playlist_view.{hpp,cpp}` compiles with no JUCE
      include, and the geometry functions are `constexpr`-free plain functions as specified.
- [ ] `tests/test_playlist_view.cpp` (target `op_playlist_view_tests`) with **at least 25 CHECKs**, every
      name prefixed `playlist.`, covering at least: `playlist_x` (0, half bar, negative, NaN, exactly
      16 bars, past the view, half-away-from-zero rounding); `bars_at_frame` (0 and negative frames,
      `length_bars == 0`, one bar and half a bar at constant tempo within `1e-4`, parking at the end,
      the `tempo.opj`-style tempo change giving `5.0` for frame 544000 and NOT the constant-bpm value,
      the within-1-sample round trip over many frames, monotonicity); `playlist_blocks` (empty
      arrangement, one entry per clip in model order, lane = pattern track, `start_bar`,
      `repeats * extent` length, `pattern_extent` as the divider spacing, trim setting the drawn
      length and its fades clamping, a divider exactly at `x1` skipped, minimum width 2 px, clamping
      to `kPlaylistRight`, a block at `start_bar 16.0` not drawn, invalid pattern index and invalid
      lane not drawn, labels (`Pattern <n>` and a pattern id) and `label_x`/`label_w`, `offset_beats`
      changing nothing, `beats_per_bar` scaling, and the input project unmodified).
- [ ] `bash harness/run_goldens.sh` etc. unchanged: no golden, no render, no CLI behaviour changes
      (`opd` is untouched).
- [ ] `dhunzilla --snapshot out.png tests/fixtures/goldens/clip_fades.opj` (1100x700, the two WP-0143
      stdout lines unchanged) shows, with `playlist_top_ = 204`: lane background `(272..1039, 204..234)`,
      `kSeparator` bar lines at x 272, 320, ..., 992, clip 0 filled `272..307` at y `207..232` with the
      repeat divider at x 290, the fade-in wedge lane-coloured at (274, 208) and clip-filled at
      (280, 210), the fade-out wedge lane-coloured at (303, 209) and clip-filled at (300, 231), clip 1
      filled `416..433` (nothing at 434), the playhead amber at (272, 204) and (273, 234) and 2 px wide
      (274, 215) is not amber, and x 1040 is the window background.
- [ ] The channel rack (WP-0195) is pixel-for-pixel unchanged; the mixer rows, transport and status
      line are unchanged.
- [ ] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_playlist_view_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, and the
      sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_playlist_view_tests`; the app target
      builds with no warnings from our sources.
- [ ] The owner screenshot set (three PNGs) is looked at and listed in the Handoff.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds the row), golden fixtures (`tests/fixtures/`), `harness/`, `.github/`,
  `docs/`, `ai-sidecar/`, `tests/test_main.cpp`, `src/engine/`, `src/cli/`, `src/doc/`,
  `src/app/main.cpp` (no new CLI options, no new script commands), `src/app/player*`,
  `src/app/playback_controller.*`, `src/app/render_playback.*`.
- No editing of any kind: no click, drag, resize, split, select, hover, scrolling or zoom; no
  `Document::apply` from the panel. No bar ruler with numbers, no clip-gain display, no drawing of
  `offset_beats`, no synthesizing blocks for the legacy empty arrangement, no tempo-map drawing, no
  per-clip colour changes, no audio or render changes, no new dependencies.
- The playhead is verified headlessly at frame 0 only (a snapshot has no audio controller): the
  `timerCallback`/`position_frames` wiring is one `repaint()` line plus the read in `paintPlaylist`,
  and is reviewed by the owner, not by the automated check. Do not add a snapshot option to fake a
  playhead position.

## Git
- Branch `wp-0196-app-playlist-panel-read` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0196: App playlist view (read-only) - arrangement clips as blocks on per-track lanes over a bar grid, playhead from the dzplayer sample counter`. Stage explicit paths only (never `git add -A`).
  Include `TASKS/WP-0196-app-playlist-panel-read.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (6 files, nothing else)
- **`src/app/playlist_view.hpp` (new)** — the spec constants, `dz::PlaylistBlock`, and the plain
  (non-`constexpr`) functions `playlist_x`, `bars_at_frame`, `playlist_blocks`. Includes only
  `<string>`, `<vector>`, `"engine/model.hpp"`. GUI-free by token: `grep -in "juce"` over both new
  module files → no matches (`exit 1`).
- **`src/app/playlist_view.cpp` (new)** — the implementation, exactly the specified maths:
  llround half-away-from-zero for `playlist_x`; 60-step bisection over `op::sample_at_bar` (its own
  extra include of `engine/render.hpp` — the renderer's tempo map, never re-derived) for
  `bars_at_frame`; model-order blocks with plain `beats_per_bar` bar↔beat maths, divider/fade/label
  rules as written. No I/O, no clock, no mutation.
- **`tests/test_playlist_view.cpp` (new)** — own `main`/`CHECK`, **49 CHECKs**, every name prefixed
  `playlist.`, covering every required case (all projects built in code; no file I/O; the
  `clip_fades.opj` worked example replicated in code, no fixture reads).
- **`src/app/main_component.hpp`** — additive only: the "Playlist (WP-0196, read-only)" header
  paragraph, private `paintPlaylist(juce::Graphics&)`, private `int playlist_top_ = 0;`. Zero
  existing lines changed (`git diff`: 14 insertions, 0 deletions).
- **`src/app/main_component.cpp`** — additive only (84 insertions, 0 deletions): the include, the four
  colours with the exact values (`kLaneBg{0xff1a1c24}`, `kClipFill{0xff3d7ea6}`,
  `kClipRepeat{0xff2a5877}`, `kPlayhead{0xffffd23f}`), `paintPlaylist` exactly in the specified
  order (lanes → blocks → wedges as `juce::Path`s → playhead LAST), `paintPlaylist(g);` as the last
  statement of `paint()`, `playlist_top_ = rack_top_ + static_cast<int>(rack_.size()) * kRackRowHeight
  + 28;` as the last statement of `resized()`, and one trailing `repaint();` in `timerCallback()`.
  No added line contains `apply(`/`*_gesture`; `const_cast`/`std::chrono`/`steady_clock`/
  `system_clock` still appear nowhere under `src/app` (grep clean before and after).
- **`CMakeLists.txt`** — the four specified additions verbatim (7 insertions incl. the comment line):
  `target_sources(dzplayer PRIVATE src/app/playlist_view.cpp)`, the `op_playlist_view_tests` block,
  and the `DZ_OUR_TARGETS` line. The `if(DZ_BUILD_APP)` block untouched.

### How verified (all run in this worktree; dz-build skill, never raw cmake/make)
Environment note: the agent sandbox only writes under `/home/shubhankar_dgx/qwen-scratch`, so
`UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv` and
`CCACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/ccache` were exported for every build.

- **Release build (`dz_build.sh`, -j4, ccache, `-Werror` on our targets): 0 warnings**; full ctest:
  ```
  20/26 Test #20: playlist_view ....................   Passed    0.01 sec
  100% tests passed, 0 tests failed out of 26
  ```
- **`op_playlist_view_tests`: 49× `PASS playlist.*` … `0 failure(s)`, exit 0** (e.g.
  `PASS playlist.x_rounds_half_away_from_zero`, `PASS playlist.bars_at_frame_tempo_change`,
  `PASS playlist.blocks_divider_exactly_at_x1_is_skipped`,
  `PASS playlist.blocks_input_project_unmodified`; the tempo check asserts 5.0 ±1e-4 for frame
  544000 AND `bars_at_frame < 5.4`, ruling out the constant-120-bpm 5.667).
- **Sanitizers (CI recipe Debug + `-DDZ_SANITIZE=ON`, per `.github/workflows/ci.yml`;
  `DZ_BUILD_TYPE=Debug` for dz_build.sh)**: `op_playlist_view_tests` → `0 failure(s)`;
  `op_tests` → `ALL PASS (0 failures)` under ASAN+UBSAN.
  Observation (not a change, out of scope): a **Release** `-DDZ_SANITIZE=ON` build fails to compile
  the pre-existing `src/engine/render.cpp:869` with a GCC 13.3 false-positive
  `-Werror=array-bounds` on `__builtin_memmove` inside `op::pattern_mode_project` — reproducible on
  the untouched base; CI uses Debug, which is clean.
- **Harness gates** (with `build/opd` symlinked to the Release `opd`, removed after):
  ```
  run_goldens: 17 fixtures, 0 failures
  test_cli_errors: 0 failures
  test_glue_scripts: 0 failures   (rel_no_repo_strays PASS)
  ```
  `opd` is untouched; no golden, render or CLI behaviour changed.
- **App build (JUCE 9, `-DDZ_BUILD_APP=ON`): no warnings from our sources** in the full build log;
  `dhunzilla --snapshot` prints the two unchanged WP-0143 lines
  (`title: DhunZilla — <file>.opj`, `snapshot: …png 1100x700`).
- **Owner screenshot set** (paths under `/home/shubhankar_dgx/qwen-scratch/par/.snaps/WP-0196/`, 1100x700):
  - `clip_fades/snapshot.png` — `playlist_top_ = 204`; every acceptance pixel checked by script:
    lane bg `(272..1039, 204..234)` = `kLaneBg`; `kSeparator` lines at x 320…992 step 48 (x 272 is
    playhead-covered at those rows, checked amber); clip 0 fill at (280,210), repeat divider
    (290,210) = `kClipRepeat`, fade-in wedge lane at (274,208), fade-out wedge lane at (303,209),
    clip fill at (300,231); clip 1 filled at (416,215)/(433,215) and lane bg at (434,215); playhead
    amber column x 272–273 over y 204..234 exactly (2 px), (274,215) not amber (inside the fade-in
    wedge, `kLaneBg`); x 1040 = window bg; "Playlist" caption text present at y 184..201.
  - `arranged/snapshot.png` — `playlist_top_ = 396`; three lane backgrounds at y 396/428/460,
    amber playhead exactly y 396..490, overlapping clip blocks present on lanes 0/1/2
    (fill-pixel counts 2576/4679/3503 in the block bands), x 1040 window bg.
  - `clip_trim/snapshot.png` — `playlist_top_ = 284`; trimmed clip 0 ends at x 344 (fill at 336,
    fade-out wedge at (340,300), lane bg at 345), divider at 320; clip 1 dividers at x 392 and 416
    (the r=2 divider of a 2-beat extent starting at bar 2), playhead y 284..346 (2 lanes).
  All pixel probes: `TOTAL FAILURES: 0`. Honest note: this implementing agent's model has **no image
  input**, so "looked at" was done as exhaustive pixel/geometry inspection (the script above), not by
  eye — the owner screenshot review should confirm the visual gestalt.
- **Channel rack (WP-0195) pixel-identity evidence**: rebuilt `dhunzilla` from the untouched base
  (the three modified files stashed, same app build dir, same fixtures) and diffed the PNGs:
  ```
  clip_fades: full-diff bbox=(16,188,1040,235) changed_px_ABOVE_playlist(y<184)=0 -> PASS
  arranged:   full-diff bbox=(16,380,1040,491) changed_px_ABOVE_playlist(y<376)=0 -> PASS
  clip_trim:  full-diff bbox=(16,268,1040,347) changed_px_ABOVE_playlist(y<264)=0 -> PASS
  ```
  Every changed pixel lies inside the new panel (caption + lanes); transport, mixer rows, channel
  rack and the status line are pixel-for-pixel identical to the base build.

### Acceptance criteria
All checked (see the evidence above): 6 files only; JUCE-free module; 49 CHECKs prefixed `playlist.`
covering every listed case; goldens/CLI unchanged; snapshot pixel set with `playlist_top_ = 204`;
channel rack pixel-identical; standard gates green incl. sanitizers; screenshot set produced and
verified (programmatically — see the honest note).

### Metrics
- 105 added lines to existing files (CMake +3 entries/7 lines, main_component.{hpp,cpp} additive),
  an 185-line module (83 + 102), a 280-line test (49 CHECKs). No golden/fixture/harness/docs/CLI
  file touched.
- `git diff --stat` (tracked files): `3 files changed, 105 insertions(+)`, 0 deletions.

### Follow-ups / PROPOSE
- Nothing outside scope needed changing; no `PROPOSE:`.
- For whoever owns CI/build-kit tooling (no repo change): `dz_build.sh --sanitize` defaults to
  Release, which trips the pre-existing GCC-13 `-Werror=array-bounds` false positive in
  `src/engine/render.cpp` — the Debug sanitizer recipe (as in `ci.yml`) is the working one.

