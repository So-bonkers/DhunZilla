---
id: WP-0229
title: dzplayer pattern mode — a pattern-mode flag in the JUCE-free player core loops ONE named pattern instead of the arrangement
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
touches: [app, cmake, new:tests/test_player_pattern.cpp]
---

## Context - why this exists

Phase 1 transport. Live playback (WP-0153), a sample-exact loop region (WP-0200) and a click while playing
(WP-0201) exist, but the app can only ever play the **arrangement**. FL Studio-style "pattern mode" — audition
one pattern on loop — is missing from the JUCE-free playback core `dz::Player`, so the UI cannot offer it. This
package adds a **pattern-mode flag naming one pattern**: `prepare_pattern()` renders that pattern **off-thread**
with the engine's own pattern-mode path and hands the result to the callback through the hand-off `submit()`
already uses; while the flag is on, `process()` wraps the sample counter over that pattern's own length instead
of following the arrangement. The callback stays a dumb executor: copies samples only, no allocation, no lock,
no clock, no rendering. With the flag off, song-mode output stays **byte-identical to today** — asserted.

Existing code this package reuses (read it first, do **not** reimplement or change it):
`src/app/player.hpp` / `player.cpp` / `player_rt.cpp` (the WP-0153 threading contract, the WP-0200 loop-window
wrap in `process()`, the WP-0201 flag+mailbox style); `op::pattern_mode_project`, `op::render_project`,
`op::sample_at_beat` in `src/engine/render.hpp`; `op::pattern_extent`, `op::Pattern`, `op::Project` in
`src/engine/model.hpp`; `op::wav::StereoBuffer` in `src/engine/wav.hpp`; `docs/spec/project-model.md`
§Pattern mode (WP-0192), §Explicit pattern length (WP-0162), §Pattern swing (WP-0166), §Pattern ids (WP-0179),
§Loop region (WP-0155); `tests/test_player_loop.cpp` (the test style to copy); `AGENTS.md` real-time rules;
`docs/adr/0004-determinism-rules.md`.

Save this file as `TASKS/WP-0229-dzplayer-pattern-mode-playback.md` and put your handoff report at its bottom.

## Spec - normative (this task file is the contract)

### 1. Files (exactly these five; stage no others)

| file | role |
|---|---|
| `src/app/player.hpp` | modified: the four new public methods + the private pattern state; contract in comments |
| `src/app/player.cpp` | modified: control-thread side (`prepare_pattern`, `has_pattern`, flag accessors) |
| `src/app/player_rt.cpp` | modified: `process()` picks the pattern window instead of the loop window (§3 rule 4) |
| `tests/test_player_pattern.cpp` | **new**: this WP's tests (target `op_player_pattern_tests`) |
| `CMakeLists.txt` | modified: register the test target (§5), purely additive |

Do NOT touch anything else — in particular not `src/engine/**`, not `src/cli/main.cpp`, not `src/app/main.cpp`,
`main_component.*`, `playback_controller.*`, `render_playback.*`, not `docs/**`, not `tests/test_player_loop.cpp`,
`tests/test_player_click.cpp`, `tests/test_main.cpp`, `harness/**`, `tests/fixtures/**`.

### 2. Public interface added to `dz::Player` (`src/app/player.hpp`) — exact names and signatures

```cpp
    // ---- control thread (never the audio thread) ----
    bool prepare_pattern(const op::Project& p, const std::string& pattern_id, std::string& err);
    bool has_pattern() const;             // a pattern window is stored (no way back to false)
    void set_pattern_mode(bool enabled);  // the pattern-mode flag
    bool pattern_mode() const;            // the flag as last set
```

`src/app/player.hpp` already includes `<string>` and `engine/model.hpp`; add nothing to its includes. The private
state you need is a flag, the window length and one `bool` — use these names (not graded, but keep the diff
obvious):

```cpp
    std::atomic<bool> pattern_mode_{false};    // control write, one load per block
    std::atomic<long long> pattern_len_{0};    // frames per pattern statement; 0 = no window
    bool pattern_stored_ = false;              // control thread only (has_pattern())
```

**No new buffer mailbox.** The rendered pattern buffer travels through the audio mailbox that already exists
(`pending_` / `retired_` / `current_`): `prepare_pattern()` hands it over exactly the way `submit()` does. Do not
invent a second lock-free structure, and do not add a mutex.

### 3. Normative behaviour rules

1. **Defaults.** A fresh `Player` has no pattern and the flag off: `has_pattern() == false`,
   `pattern_mode() == false`, and nothing wraps. **The flag alone cannot enable a wrap**: with the flag on and
   no pattern ever prepared (`pattern_len_ == 0`), `process()` behaves bit-for-bit as it does today (WP-0153
   one-shot pass over the buffer, zero-fill past the end, then stop and park at frame 0).
2. **The flag.** `set_pattern_mode(b)` stores `b`; `pattern_mode()` returns exactly the last value stored —
   independently of whether a pattern is prepared. It never renders, allocates or frees. `prepare_pattern()`
   never enables it, `set_pattern_mode(true)` never renders: both are needed for pattern mode to bite.
3. **`prepare_pattern(p, pattern_id, err)` runs entirely on the calling (control) thread, in this order.**
   1. **Id lookup:** `pattern_id` is matched byte-for-byte against `p.patterns[i].id` (spec §Pattern ids /
      WP-0192). An **empty `pattern_id` selects nothing**, so unnamed patterns cannot be auditioned. Unknown id
      → return `false`, leave `err` **non-empty and containing `pattern_id`** (wording yours; suggested
      `unknown pattern '<pattern_id>'`), and change nothing in the player: stored pattern, current buffer and
      flag stay exactly as they were.
   2. **One statement of the pattern:** `op::Project pat_proj; op::pattern_mode_project(p, index, 1, pat_proj, err);`
      — `loops` is exactly `1` (the repetition is done by the transport wrap, not by the render). On failure
      return `false` with `err` left **exactly as the engine set it** (the spec's "has no length" message) and
      change nothing.
   3. **The wrap length (normative, use the engine's own beat→sample map — do not re-derive it):**
      ```cpp
      const long long len = op::sample_at_beat(p, op::pattern_extent(p.patterns[index]));
      ```
      If `!(len > 0)` return `false` with a non-empty `err` and change nothing.
   4. **Render off-thread:** `op::wav::StereoBuffer* buf = new op::wav::StereoBuffer();` then
      `op::render_project(pat_proj, "", *buf, err)` (empty base dir — sampler-asset resolution is out of scope).
      On failure destroy the buffer, return `false` with `err` exactly as the engine set it, and change nothing.
   5. **Success:** hand the buffer over the way `submit()` does (so the pattern render **becomes** the buffer
      being played; the buffer previously in flight is destroyed here, on the control thread), store `len` in
      the window atomic, set the stored state (`has_pattern()` becomes and stays `true`; there is **no** unload /
      `clear_pattern()` — out of scope), `err.clear()`, return `true`. A later successful call replaces both the
      buffer and the window (last prepare wins). A later `submit()` replaces the buffer but keeps the window and
      `has_pattern()` true.
   The `Project` is NOT kept — later project edits are not tracked; call `prepare_pattern()` again.
4. **The wrap while the flag is on (the only new signal path).** Let `frames` be the frame count of the buffer
   `process()` has adopted and `P` the stored length. Snapshot the flag and `P` **once per block**, at the same
   place the loop bounds are snapshotted today. When the flag is on **and** `P > 0`, the loop window for this
   block is
   ```
   start = 0,  end_eff = min(P, frames)
   ```
   and the existing WP-0200 loop path does everything else, unchanged: every slot of the block is copied from
   the buffer, switching mid-block; after frame `end_eff - 1` the next read is frame `0` (no gap, no overlap, no
   silence); a block longer than the window wraps more than once; a block ending exactly on `end_eff` leaves
   `position_frames()` at `0`; playback never ends — only `stop()` ends it. `position_frames()` lies in
   `[0, min(P, frames))` after the first wrap, and `frames_played()` grows by exactly `num_frames` per block and
   is never reset by a wrap. **Byte-for-byte target:** with entry position 0, output slot `g` carries buffer
   frame `g mod P`.
5. **Clamping.** `P` is clamped to the current buffer exactly like an out-of-range loop end already is: a buffer
   shorter than `P` wraps at its own frame count; a longer buffer keeps `P` (the frames past `P` are never read).
   Nothing is validated against a buffer at `prepare_pattern()` time.
6. **Pattern mode wins over the loop region.** While the flag is on and `P > 0`, `process()` does not consult the
   loop region **at all** — a window set with `set_loop()` is ignored for those blocks. `set_loop()` /
   `clear_loop()` / `has_loop()` keep their state and their WP-0200 meaning: with the flag off, the loop region
   wraps exactly as before, even over a pattern buffer. Turning the flag **off mid-playback** neither seeks nor
   silences (same contract as `clear_loop()`): playback runs on from the current position — under the loop region
   if one is set, otherwise one-shot to the end of the buffer — then stops and parks at frame 0.
7. **Song mode stays byte-identical.** With the flag off (or with no pattern ever prepared), `process()` output,
   `position_frames()` and `frames_played()` must be bit-identical to the same calls made without this work
   package. `tests/test_player_loop.cpp` and `tests/test_player_click.cpp` stay untouched and must still pass.
8. **Transport unchanged.** `play()` still starts at frame 0 of the buffer being played — no seek to any window
   start; `stop()` silences and parks at 0; `playing()` stays `true` through every wrap.
9. **Click interaction unchanged.** WP-0201 rule 6 keeps holding: the click (if armed and enabled) is indexed by
   the **wrapped source frame** the slot reads its audio from. Pattern mode renders no pattern-specific click and
   changes nothing else in the click path.
10. **Timing.** A control-thread change (flag, window, buffer) takes effect no later than the next `process()`
    call; the callback reads the flag and the length once per block.
11. **Ownership / no leaks.** Every pattern buffer is destroyed on the control thread only — inside
    `prepare_pattern()`'s hand-off, `submit()`, `reclaim()` or `~Player` — never in `process()`. ASAN/LSAN must
    report no leak.
12. **Determinism (ADR-0004).** No wall clock, no randomness, no new engine paths, no change to `opd render` or
    the goldens (they never touch `dz::Player`). With a project that has a `tempo_map`, `P` is still the fixed
    frame count of the **first** statement (rule 3.3) — later statements are not re-mapped. That is a documented
    limitation, not a bug to fix here.

### 4. Audio-callback purity (`src/app/player_rt.cpp`) — graded by grep

`process()` keeps living in this file: the definition stays `Player::process(float*, float*, int)` in
`src/app/player_rt.cpp` — do not move the callback to another file or into a header. The file must contain
**none** of these tokens, in code *or* comment prose (`grep -nE` with this exact alternation — the same rule as
WP-0200/WP-0201):

```
std::mutex|std::lock_guard|std::unique_lock|condition_variable|std::chrono|steady_clock|system_clock|high_resolution_clock|gettimeofday|\bnew\b|\bdelete\b|\bmalloc\b|\bfree\b|printf|cout|cerr|fopen|ofstream|ifstream|push_back|resize|throw
```

So write comments like "the previous buffer is destroyed by the control thread", never "delete the old buffer";
name a local `plen`, not `new_len`. Only atomic loads, plain loops, integer `std::min`/`std::max` and float
copies belong in here. The whole loop-wrap path of WP-0200 is **reused**, not duplicated.

### 5. `CMakeLists.txt` — exact additions (additive only)

After the WP-0201 block (`op_player_click_tests` / `add_test(NAME player_click …)`):

```cmake
# WP-0229: dz::Player pattern mode — one named pattern looped instead of the arrangement.
add_executable(op_player_pattern_tests tests/test_player_pattern.cpp)
target_link_libraries(op_player_pattern_tests PRIVATE opengine dzplayer)
add_test(NAME player_pattern COMMAND op_player_pattern_tests)
```

and immediately after the existing `list(APPEND DZ_OUR_TARGETS op_player_click_tests)  # WP-0201` line:

```cmake
list(APPEND DZ_OUR_TARGETS op_player_pattern_tests)  # WP-0229
```

Target name `op_player_pattern_tests`, CTest name `player_pattern` — verbatim.

### 6. `tests/test_player_pattern.cpp` (target `op_player_pattern_tests`)

Own `main`, assert-based, no framework, **no file I/O and no wall clock**; drive the player with simulated
device callbacks of **128 frames** on the test thread exactly like `tests/test_player_loop.cpp`; exit code =
number of failures. At least **12 `CHECK(...)` calls**.

**The reference is the engine's own pattern-mode render** — what `opd render <proj> -o out.wav --pattern NAME
--loops N` writes (spec §Pattern mode, WP-0192) — built in memory with
`op::pattern_mode_project(p, index, N, pm, err)` + `op::render_project(pm, "", ref, err)`. Every comparison is
**bit-exact** (`memcmp` / `==` on the floats), **never a tolerance**. Never shell out to `opd`, never read a
fixture.

Build the project in code, so the byte-comparison is exact: **120 bpm, `sample_rate` 48000, `beats_per_bar` 4,
`length_bars` 4.0, `seed` 42** → 24000 samples per beat, so every 16th-step position and every swing offset is an
exact integer sample and the loops render is exactly periodic with the statement length. Tracks: a `metronome`
track (track 0) and a `sine` track (track 1). Patterns:

- **`A`** (track 0): `length_beats = 3.0`, `swing = 0.25`, notes on a 16th grid across the statement
  (12 notes, `t_beats = 0.25*i`, `dur_beats = 0.25`) → statement length `P = 72000` frames, the loops=1 buffer
  is `96000` frames (padded to a whole bar), so **`P < frames`** and `P % 128 = 64` (wraps never land on a block
  edge — assert both as preconditions).
- **`B`** (track 0): **no** `length_beats` (extent derived from its notes: 1.75 beats → `P = 42000`), `swing = 0.5`.
- **unnamed** (`id == ""`, on the `sine` track): never selectable, and silent in pattern mode.

Give the project a non-empty `arrangement` (a clip of `A`, repeats 4, plus a clip of the unnamed pattern) so song
mode and pattern mode differ audibly. No note may reach its statement boundary: for the metronome voice the tone
is a hard 30 ms stop, for `sine` the note stops at `t + dur`; keep every `t_beats + dur_beats <= length` and
leave at least 0.1 beat of slack. Include a precondition `CHECK` that the multi-loop reference really is
periodic — `ref[f] == ref[f - P]` for every frame `f` in `[P, (N-1)*P)` — because that is what the byte-compare
rests on.

Cover at least:

- defaults: no pattern, flag off; the flag round-trips and is independent of `has_pattern()`; **flag on with no
  prepared pattern is still the plain one-shot** (rule 1).
- unknown id and empty id both fail: `false`, `err` non-empty and containing the id, `has_pattern()` still
  false (rule 3.1); a failed `prepare_pattern()` **after** a successful one leaves the stored pattern, the
  buffer and the flag untouched (prove it by playing afterwards and checking the wrap is still the good one).
- `prepare_pattern()` succeeds, `err` is cleared, `has_pattern()` is true and `pattern_mode()` is still false;
  with the flag off the buffer now playing is that **loops = 1** render, one shot, byte-identical, then zeros,
  then parked at 0 (rules 2/3.5).
- **the headline:** pattern mode on, several hundred 128-frame callbacks spanning **at least four wraps** of
  `A`, every slot bit-identical to the `loops >= 4` pattern-mode render read at frame `g mod P`; `playing()`
  stays true; `position_frames()` in `[0, P)` after the first wrap and equal to `(frames driven) mod P`;
  `frames_played()` equals the frames driven (rules 4/5).
- **pattern mode wins over a loop region that would otherwise wrap:** set `set_loop(48000, 90000)` (inside the
  96000-frame pattern buffer) *and* the flag → output follows `P = 72000`; with the flag off and the same loop
  → output follows the loop window (rules 6/7).
- song mode with the flag off is byte-identical: `prepare_pattern()` then `submit()` of a full
  `op::render_project(p, "", …)` buffer → one pass of the song render, zeros past it, parked at 0 (rule 7).
- last prepare wins: `A` then `B` → wraps at `42000` and matches `B`'s loops render (rule 3.5).
- flag **off** mid-playback: no seek, no silence; the rest of the buffer runs out one-shot (byte-identical),
  then `playing()` false, `position_frames() == 0`, `frames_played()` unchanged by the switch (rule 6).
- clamp: with the flag on and a **shorter** submitted buffer (a 500-frame ramp) the wrap is `[0, 500)` (rule 5);
  mono (`right == nullptr`) keeps wrapping the left channel; `stop()` + `play()` restarts at frame 0 (rule 8).

## Determinism / repo rules (ADR-0004)

No wall clock, no unseeded randomness, libm only via `dsp.hpp`, no fast-math; `-Werror` clean and clean under
`-DDZ_SANITIZE=ON` (ASAN+UBSAN+LSAN). Tests use no fixed `/tmp/op_*` paths (this test needs no temp dir at all).

## Acceptance criteria

- [ ] All five files as specified; nothing else changed — `git diff --name-only main` lists exactly them plus
      `TASKS/WP-0229-dzplayer-pattern-mode-playback.md`.
- [ ] `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4` → success, **zero warnings**.
- [ ] `./build/op_player_pattern_tests` → `ALL PASS (0 failures)`, exit 0.
- [ ] `./build/op_player_loop_tests` and `./build/op_player_click_tests` → `ALL PASS (0 failures)` with both
      files untouched (rule 7 regression).
- [ ] Pattern mode is byte-exact: the 128-frame callback stream over at least four wraps equals the samples of
      `opd render --pattern A --loops N` (the engine pattern-mode render) at `g mod P`, `memcmp`-equal, no
      tolerance; the loop window is the pattern's **own** length `P`, not the rendered buffer's length.
- [ ] `./build/op_tests` → `ALL PASS (0 failures)`; `bash harness/run_goldens.sh` → `run_goldens: 16 fixtures,
      0 failures`; `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`;
      `UV_CACHE_DIR=<workspace>/.cache/uv bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
- [ ] Sanitizers: `cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san
      -j4 --target op_player_pattern_tests op_tests`, then run both → all pass, no ASAN/UBSAN/LSAN report.
- [ ] The §4 purity grep over `src/app/player_rt.cpp` prints nothing (exit 1).
- [ ] Evidence (commands + output tails) in the Handoff report below.

## Out of scope - do NOT touch

- No CLI surface: `opd render --pattern` (WP-0192) is the reference and stays as it is; `src/cli/main.cpp` and
  `opd --render-playback` (`src/app/render_playback.*`) gain no pattern flag.
- No UI/JUCE wiring: `src/app/main.cpp`, `main_component.*`, `playback_controller.*` untouched — no pattern-mode
  button, no combo box, no device changes.
- No changes to `src/engine/**` — reuse `pattern_mode_project`, `render_project`, `sample_at_beat`,
  `pattern_extent` exactly as they are. No new engine module.
- No pattern-specific click re-render, no per-pattern click, no count-in, no pre-roll, no crossfade at the wrap,
  no seek to the window start, no "start at bar" option, no pattern-length editing, no tempo-map-aware
  multi-statement windowing (see §3 rule 12).
- No `clear_pattern()` / pattern unload, no pattern mode over a file or an arbitrary submitted buffer.
- No `docs/spec/**` change: like WP-0200's loop region and WP-0201's click, the player contract lives in
  `src/app/player.hpp` (the render-side contract is already §Pattern mode, WP-0192).
- Do NOT edit `ROADMAP.md`; no new/changed golden fixture in `tests/fixtures/`; no new dependencies; do not edit
  `tests/test_main.cpp`, `tests/test_player_loop.cpp` or `tests/test_player_click.cpp`.

## Git
- Branch `wp-0229-dzplayer-pattern-mode-playback` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0229: dzplayer pattern mode — one named
  pattern looped instead of the arrangement`. Stage explicit paths only (never `git add -A`).
  Include `TASKS/WP-0229-dzplayer-pattern-mode-playback.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the five §1 files + this task file)
- `src/app/player.hpp` — the four public methods verbatim (`prepare_pattern` / `has_pattern` /
  `set_pattern_mode` / `pattern_mode`) with the full §3 contract in comments; the three named private
  members (`pattern_mode_`, `pattern_len_`, `pattern_stored_`). No new includes, **no new mailbox, no
  mutex** — the pattern render travels through `pending_`/`retired_`/`current_` exactly like `submit()`.
  Threading-contract header line extended with the four method names.
- `src/app/player.cpp` — `prepare_pattern()` in the exact §3 rule 3 order: byte-exact id lookup
  (empty id selects nothing → `unknown pattern '<id>'`), `op::pattern_mode_project(p, index, 1, …)`,
  `len = op::sample_at_beat(p, op::pattern_extent(p.patterns[index]))` with the `!(len > 0)` guard,
  `new op::wav::StereoBuffer` + `op::render_project(pat_proj, "", *buf, err)`; every failure path
  destroys its own buffer and changes nothing (engine `err` kept verbatim). Success: the `submit()`
  exchange (`delete pending_.exchange(buf); reclaim();`), window atomic, stored flag, `err.clear()`.
- `src/app/player_rt.cpp` — snapshot `pattern_mode_` + `pattern_len_` at the same place the loop
  bounds are snapshotted; when flag && `plen > 0` the window becomes `start = 0, end_eff =
  std::min(plen, frames)` and the **entire existing WP-0200 wrap path does everything else**.
  Flag or `plen == 0` → WP-0200/WP-0153 paths byte-unchanged.
- `tests/test_player_pattern.cpp` — new, 21 `CHECK`s, own main, no framework / file I/O / wall clock,
  128-frame simulated callbacks; reference = in-memory `op::pattern_mode_project +
  op::render_project` (the `opd render --pattern --loops` code path), all `memcmp`/`==` bit-exact.
- `CMakeLists.txt` — purely additive: the §5 block verbatim after the WP-0201 block, and
  `list(APPEND DZ_OUR_TARGETS op_player_pattern_tests)  # WP-0229` right after the WP-0201 line.

### How verified (all commands run in the worktree; real tails below)
Note on the build command: per the dz-build kit this repo is built with
`bash agent-build-kit/loop/tools/dz_build.sh` (shared build lock, `-j4`, ccache), never raw
`cmake --build`; it runs exactly the acceptance configure (`-DCMAKE_BUILD_TYPE=Release`, Debug with
`DZ_BUILD_TYPE=Debug` for `build-san`) into `--out "$PWD/build"` / `"$PWD/build-san"`. In this
sandbox `CCACHE_DIR` must point at the workspace (`/home/shubhankar_dgx/qwen-scratch/.cache/ccache`);
`UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv` for the glue run.

1. `-Werror` Release build, zero warnings → exit 0; `grep -ci "warning" build/dz_build.log` → `0`.
   ```
   BUILD_EXIT=0        # dz_build.sh … --out "$PWD/build"; last line: <worktree>/build
   ```
2. `./build/op_player_pattern_tests` → `ALL PASS (0 failures)`, exit 0 (21 PASS lines, incl. the
   headline byte-exactness over 4+ wraps at `g mod P`, the P=72000/frames=96000/`P % 128 = 64` and
   `ref[f] == ref[f-P]` periodicity preconditions):
   ```
   PASS pat.headline_pattern_output_is_byte_exact_over_four_wraps
   PASS pat.headline_counters_hold_through_every_wrap
   PASS pat.pattern_mode_ignores_the_loop_region_while_the_flag_is_on
   PASS pat.with_flag_off_the_loop_region_wraps_as_before
   PASS pat.song_mode_after_prepare_is_byte_identical_one_pass
   PASS pat.last_prepare_wins_wraps_at_Bs_extent
   PASS pat.flag_off_mid_playback_runs_out_one_shot_then_parks
   PASS pat.short_submitted_buffer_clamps_the_pattern_window_and_mono_wraps
   PASS pat.stop_then_play_restarts_at_frame_zero
   ALL PASS (0 failures)
   exit=0
   ```
3. Regression, files untouched: `./build/op_player_loop_tests` → `ALL PASS (0 failures)`;
   `./build/op_player_click_tests` → `ALL PASS (0 failures)`; both `exit=0`.
4. `./build/op_tests` → `ALL PASS (0 failures)`. `bash harness/run_goldens.sh` →
   `run_goldens: 17 fixtures, 0 failures` (the task text says 16 — the integration base already
   carries 17 golden fixtures, e.g. the WP-0215 bundled kit project; **0 failures** is the contract).
   `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
   `UV_CACHE_DIR=… bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
5. Sanitizers: dz_build.sh `--sanitize` with `DZ_BUILD_TYPE=Debug` (= `-DCMAKE_BUILD_TYPE=Debug
   -DDZ_SANITIZE=ON`) into `build-san`, targets `op_player_pattern_tests op_tests` → exit 0; both
   binaries run: `op_player_pattern_tests: exit=0  tail: ALL PASS (0 failures)`,
   `op_tests: exit=0  tail: ALL PASS (0 failures)`, and `grep -cE
   "AddressSanitizer|runtime error|LeakSanitizer|SEGV"` over each run's full output → `0` (so no
   ASAN/UBSAN/LSAN report, i.e. no leak — every pattern buffer dies on the control thread).
6. §4 purity grep (verbatim alternation from the task) prints nothing, exit 1:
   ```
   $ grep -nE 'std::mutex|std::lock_guard|std::unique_lock|condition_variable|std::chrono|steady_clock|system_clock|high_resolution_clock|gettimeofday|\bnew\b|\bdelete\b|\bmalloc\b|\bfree\b|printf|cout|cerr|fopen|ofstream|ifstream|push_back|resize|throw' src/app/player_rt.cpp
   grep exit: 1
   ```
7. Scope: `git diff --name-only main` (pre-commit) listed exactly
   `CMakeLists.txt src/app/player.cpp src/app/player.hpp src/app/player_rt.cpp`, plus the untracked
   `tests/test_player_pattern.cpp` and this task file — nothing else. `playback_controller.*`,
   `main_component.*`, `render_playback.*`, `src/cli/main.cpp`, `src/engine/**`, `docs/**`,
   `harness/**`, `tests/fixtures/**`, and the three protected test files are untouched (git status).

### Metrics
- Diff: +~134 lines in the three `src/app` files (all comments+contract included), +6 lines
  CMakeLists, ~680-line new test file with 21 CHECKs (≥12 required) — 4 preconditions, defaults,
  both failure classes, state-intact-after-failure, loops=1 one-shot, the ≥4-wrap headline, loop
  vs pattern precedence both ways, song-mode byte-identity, last-prepare-wins, flag-off mid-run
  run-out, clamp+mono+restart.
- Headline drive: 2255 × 128-frame blocks = 288640 frames > 4 × 72000; wraps never land on a block
  edge for P (P % 128 = 64) yet the exactly-aligned 4P boundary leaves `position_frames() == 0`
  (asserted); `frames_played()` grows +128/block, never resets.

### Follow-ups / notes
- No PROPOSE: nothing outside scope needed to change. Tempo-map single-statement windowing stays a
  documented limitation (§3 rule 12, restated in `player.hpp`); UI wiring of the flag is the next
  package's job as planned.
- Goldens count note: harness reports 17 fixtures on this base, not 16 (see 4 above).

