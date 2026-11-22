---
id: WP-0201
title: dzplayer metronome click during playback — a click-enable flag in the JUCE-free player core mixes the project's `opd click` track while playing
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, app, new:tests/test_player_click.cpp]
---

## Context — why this exists

Phase 1 transport. Live playback exists (WP-0153) and the player already honours a loop region (WP-0200), but
there is **no click while playing**: today a metronome means hand-placing notes on a `metronome` voice, or
rendering a separate WAV with `opd click` (WP-0148) and lining it up by hand. This package puts the click in
the JUCE-free playback core `dz::Player`: **a click-enable flag** that mixes the project's own click track
into whatever the audio callback copies out, so Play gives you a metronome that follows the project's tempo
map — and loops with the transport.

The click is **pre-rendered off-thread**. `prepare_click()` calls the EXISTING engine function
`op::render_click` (the same code `opd click` runs) on the calling thread, so the onsets come from the
project's own beat→sample mapping and the accent rule (1500 Hz downbeat / 1000 Hz beat, WP-0148) is
identical *by construction* — nothing in `src/app/` re-derives a click. `process()` keeps being a dumb
executor: it only reads pre-rendered samples and adds them. No rendering, no allocation, no lock, no clock
inside the callback. **With the flag off the output stays byte-identical to today**, which is asserted.

Read first (do not change them beyond `src/app/player.*`): `src/app/player.hpp` (threading contract + WP-0200
loop rules), `src/app/player.cpp`, `src/app/player_rt.cpp` (the callback), `src/engine/render.hpp`
(`beat_onsets`, `render_click`, `total_samples_for`), `src/engine/wav.hpp` (`StereoBuffer`),
`docs/spec/project-model.md` §Click track render (WP-0148), `tests/test_player_loop.cpp` (the test style to
copy), `AGENTS.md` real-time rules, `docs/adr/0004-determinism-rules.md`.

Save this file as `TASKS/WP-0201-dzplayer-metronome-during-playback.md` and put your handoff report at its
bottom.

## Spec — what "correct" means

### 1. Files (exactly these five, nothing else)

| file | role |
|---|---|
| `src/app/player.hpp` | modified: the four new public methods + the private click state; contract in comments |
| `src/app/player.cpp` | modified: control-thread side (`prepare_click`, `has_click`, flag accessors, destructor frees the click buffers) |
| `src/app/player_rt.cpp` | modified: `process()` adopts a click track and mixes it (dumb executor, §4) |
| `tests/test_player_click.cpp` | **new**: the WP's tests (target `op_player_click_tests`) |
| `CMakeLists.txt` | modified: register the test target (§5), additive only |

Do NOT touch anything else — in particular not `src/engine/*` (`render.cpp`/`render.hpp` stay as they are),
not `src/cli/main.cpp`, not `src/app/main.cpp`, `main_component.*`, `playback_controller.*`,
`render_playback.*`, not `docs/**`, not `tests/test_player_loop.cpp`.

### 2. Public interface added to `dz::Player` (`src/app/player.hpp`) — exact names and signatures

```cpp
    // ---- control thread (never the audio thread) ----
    bool prepare_click(const op::Project& p, std::string& err);  // renders OFF-THREAD and stores the track
    bool has_click() const;                                      // a click track is stored
    void set_click_enabled(bool enabled);                        // the click-enable flag
    bool click_enabled() const;                                  // the flag as last set
```

`src/app/player.hpp` must `#include <string>` and `#include "engine/model.hpp"` (for `op::Project`) in
addition to what it already includes. The private state you need is one flag plus a second buffer mailbox;
use these names (not graded, but keep the diff obvious):

```cpp
    std::atomic<bool> click_enabled_{false};                      // control write, one load per block
    bool click_stored_ = false;                                   // control thread only (has_click())
    std::atomic<const op::wav::StereoBuffer*> click_pending_{nullptr};   // control → audio
    std::atomic<const op::wav::StereoBuffer*> click_retired_{nullptr};   // audio → control
    const op::wav::StereoBuffer* click_current_ = nullptr;               // owned by the audio thread
```

### 3. Normative behaviour rules

1. **Defaults.** A fresh `Player` has no track and the flag off: `has_click() == false`,
   `click_enabled() == false`, and nothing is ever mixed.
2. **The flag.** `set_click_enabled(b)` stores `b`; `click_enabled()` returns exactly the last value stored —
   independently of whether a track exists. It never renders, allocates or frees. Mixing needs BOTH the flag
   on *and* a stored track; there is no implicit enable in `prepare_click`.
3. **`prepare_click(p, err)` runs entirely on the calling (control) thread.** It creates a
   `op::wav::StereoBuffer`, calls `op::render_click(p, track, err)` and nothing else:
   - success → the track becomes the stored track (replacing any previous one), `err` is set to the empty
     string, return `true`;
   - failure → return `false` with **both** the stored track and the flag left exactly as they were, and
     `err` left exactly as `op::render_click` set it (do not invent or rewrite messages).
   The Project is NOT kept: later project changes are not tracked — call `prepare_click` again.
4. **`has_click()`** is true from the first successful `prepare_click` on; a later successful one replaces the
   track and keeps it true. There is no way back to false — do **not** add a `clear_click()`/unload method
   (out of scope).
5. **Per-block decision.** A block is mixed iff the flag is on AND a click track has been adopted by
   `process()`. Both are read once per block, at the start of the copy phase.
6. **Per-slot mixing (the only signal path).** For an output slot whose source buffer frame index is `f` —
   exactly the index the slot reads its audio from, i.e. `f = pos + i` on the one-shot path and the *wrapped*
   index on the loop path (WP-0200) — write
   `left[i]  = data[2*f] + click[2*f]` and, when `right != nullptr`,
   `right[i] = data[2*f+1] + click[2*f+1]`.
   That is **one float addition per channel**: no gain, no scaling, no clamping/saturation, no dithering, no
   cross-channel mixing (left never sees the right click channel). If `f >= click_frames` (the track is
   shorter than the buffer) nothing is added for that slot. A mono call (`right == nullptr`) reads/writes
   only the left channel — do not read `click[2*f+1]`.
7. **No click where nothing was copied.** Every slot of a block produced while `!playing()` or with no audio
   buffer adopted is exactly `0.0f`, and so is the zero-filled tail past the end of the buffer on the
   one-shot path. The transport stopped ⇒ silence, even with the flag on and a track armed.
8. **Under a loop the click follows the wrapped frame** (rule 6), so bursts re-fire at the wrapped onsets on
   every round. With entry position 0 and window `[S, E)` the source frame of output slot `g` is
   `f(g) = g` for `g < E`, else `S + (g - E) mod (E - S)` — the same closed form WP-0200 defines. The click
   track is indexed by that wrapped `f`, never by the output slot or a running counter.
9. **Byte-identity with the flag off.** With `click_enabled() == false` — or with no track ever prepared —
   `process()` must produce bit-identical samples to the same calls made without this work package, and
   `position_frames()` / `frames_played()` must advance exactly as before. `tests/test_player_loop.cpp`
   stays untouched and must still pass.
10. **No resampling, no rate check.** The click track is read frame-for-frame against the buffer; its
    `sample_rate` field is not inspected (callers pass a project whose rate matches the buffer, which is what
    `op::render_click` produces for it). Reads are bounded by rule 6.
11. **Ownership / no leaks.** Every click buffer is destroyed on the control thread only — inside
    `prepare_click` or `~Player` — never in `process()`. Use exactly the single-slot `exchange` handshake
    `submit()`/`process()` already use for audio (pending → current, previous handed back through retired and
    freed by the control thread); invent no new lock-free structure. At most two click tracks are alive at a
    time; ASAN/LSAN must report no leak.
12. **Timing.** When `submit()`, `prepare_click()` and `set_click_enabled(true)` all happen before the very
    first `process()` call, mixing is exact from frame 0 of that first block. Otherwise a change takes effect
    no later than the next `process()` call (the callback adopts a pending track in its hand-off at the start
    of a block and snapshots the flag once per block).
13. **Determinism.** No wall clock, no randomness, no new engine paths; nothing that could make
    `opd render` or the goldens change (they never touch `dz::Player`).

### 4. Audio-callback purity (`src/app/player_rt.cpp`) — graded by grep

Same rule as WP-0153 §6: the file must contain **none** of these tokens, in code *or* comment prose
(`grep -nE` with this exact alternation):

```
new|delete|malloc|calloc|realloc|free\(|std::vector|std::string|std::mutex|std::lock_guard|std::unique_lock|std::condition_variable|printf|std::cout|std::cerr|fopen|std::ofstream|std::ifstream|std::this_thread|sleep|push_back|resize|throw
```

So in `player_rt.cpp` write e.g. "the dropped track is destroyed by the control thread", never "delete"/"free
it"; name locals `fresh_click`, not `new_click`. Only atomic loads/exchanges, plain loops, integer arithmetic
and float additions belong in here.

### 5. `CMakeLists.txt` — exact additions (additive only)

After the WP-0200 block (`op_player_loop_tests` / `add_test(NAME player_loop …)`):

```cmake
# WP-0201: dz::Player metronome click mixed in while playing (dzplayer core + engine click track).
add_executable(op_player_click_tests tests/test_player_click.cpp)
target_link_libraries(op_player_click_tests PRIVATE opengine dzplayer)
add_test(NAME player_click COMMAND op_player_click_tests)
```

and immediately after the existing `list(APPEND DZ_OUR_TARGETS op_player_loop_tests)  # WP-0200` line:

```cmake
list(APPEND DZ_OUR_TARGETS op_player_click_tests)  # WP-0201
```

Target name is `op_player_click_tests`, test name `player_click` — verbatim.

### 6. `tests/test_player_click.cpp` (target `op_player_click_tests`)

Own `main`, assert-based, no framework, **no file I/O and no wall clock**; drive the player with simulated
device callbacks of 128 frames on the test thread exactly like `tests/test_player_loop.cpp` does; exit code =
number of failures. Use projects built in code with timing fields only (the click ignores tracks/patterns/
gains) and use the engine's own `op::render_click` / `op::beat_onsets` output as the reference — compared
**bit-exactly** (`==`, never a tolerance). At least **8 `CHECK(...)` calls**, covering:

- defaults + `prepare_click` keeps the flag off, and the flag round-trip;
- a failing `prepare_click` (e.g. `length_bars = 0.0`) returns false with a non-empty `err` and leaves track
  and flag unchanged (`has_click()` still true);
- silent audio + click enabled == the `op::render_click` buffer bit-for-bit, its burst starts are exactly
  `op::beat_onsets(p)` and each onset sample is `0.5f`;
- mixing over a non-silent buffer is exactly `audio + click` per channel (and flag off == plain audio);
- loop wrap: window `[S, E)` with `S`, `E` not multiples of 128, several rounds — every slot equals the
  reference click at the wrapped frame, and burst starts re-fire at the wrapped beat positions;
- **a loop window shorter than one callback block** (e.g. `[0, 97)` with 128-frame callbacks): it wraps inside
  *every* block, so almost every slot exercises the wrap. This is the case that catches an implementation
  indexing the click by `pos + i` (or any running output counter) instead of by the wrapped source frame — a
  window longer than a block only stresses a handful of slots per round and lets that bug through;
- a block before `play()` and the block after `stop()` are exactly zero with a track armed and the flag on.

## Acceptance criteria

- [ ] All five files as specified; nothing else in the repo changed (`git diff --name-only main` lists only
      them plus `TASKS/WP-0201-dzplayer-metronome-during-playback.md`).
- [ ] `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4` → success, **zero warnings**
      (our targets are `-Werror`).
- [ ] `./build/op_player_click_tests` → `ALL PASS (0 failures)`, exit 0.
- [ ] `./build/op_player_loop_tests` → `ALL PASS (0 failures)` with the file untouched (rule 9 regression).
- [ ] `./build/op_tests` → `ALL PASS (0 failures)`; `bash harness/run_goldens.sh` → `run_goldens: 15 fixtures,
      0 failures`; `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`;
      `UV_CACHE_DIR=<workspace>/.cache/uv bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
- [ ] Sanitizers: `cmake -S . -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san
      -j4 --target op_player_click_tests op_tests` then run both → all pass, no ASAN/UBSAN/LSAN report.
- [ ] `grep -nE '<the §4 alternation>' src/app/player_rt.cpp` prints nothing (exit 1).
- [ ] Evidence (commands + output tails) in the Handoff report below.

## Out of scope

- No UI, no JUCE changes: `src/app/main.cpp`, `main_component.*`, `playback_controller.*` are untouched — no
  toolbar click button, no device wiring.
- No CLI surface: `opd` and `--render-playback` (`src/app/render_playback.*`) gain no click flag; `opd render`
  output is unchanged (it never uses `dz::Player`).
- No click level/gain/pan, no per-channel or per-track click, no count-in, no pre-roll, no ducking.
- No `clear_click()` / track unload, no click on a *file* or arbitrary buffer — only "the project's click".
- No resampling, sample-rate validation or length matching between the click track and the audio buffer.
- No changes to `src/engine/*` (including `render.cpp`/`render.hpp`), `src/cli/main.cpp`, `src/doc/*`.
- No `docs/spec/**` change: like WP-0200's loop region, the player contract lives in `src/app/player.hpp`.
- Do NOT edit `ROADMAP.md`; do not add or modify any golden fixture in `tests/fixtures/`; no new dependencies.

## Git

Branch `wp-0201-dzplayer-metronome-during-playback` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0201: dzplayer metronome click
during playback`. Never push or merge.

## Handoff

**Branch:** `wp-0201-dzplayer-metronome-during-playback` from the integration base HEAD (`a94c482`, WP-0200). Not pushed, not merged.

### What changed (exactly the five files of §1)

| file | change |
|---|---|
| `src/app/player.hpp` | +`#include <string>`, +`#include "engine/model.hpp"`; the four public methods (`prepare_click`/`has_click`/`set_click_enabled`/`click_enabled`) with the §2/§3 contract in comments incl. a usage example; private click state exactly as §2 names it (`click_enabled_`, `click_stored_`, `click_pending_`, `click_retired_`, `click_current_`); threading-contract header line extended to list the new control-thread methods |
| `src/app/player.cpp` | +`#include "engine/render.hpp"`; `prepare_click` renders off-thread via `op::render_click` only (failure: buffer dropped, stored track + flag untouched, engine `err` verbatim; success: single-slot exchange into `click_pending_`, retiree collected here, `err.clear()`); `has_click`/`set_click_enabled`/`click_enabled`; destructor now also destroys both click slots and `click_current_` on the control thread |
| `src/app/player_rt.cpp` | `process()` adopts a pending click track with the same single-slot exchange handshake as audio (retire slot must be empty; adopted-out track handed back through `click_retired_`, destroyed by the control thread); `mix_click = flag && adopted` read once per block at the start of the copy phase; both copy paths add exactly one float per channel at the source frame index — `pos + i` one-shot, wrapped `p` under a loop (§3 rules 6/8) — guarded by `f < click_frames`; silence/tail-zero paths untouched; mono path never reads `click[2*f+1]` |
| `tests/test_player_click.cpp` | **new**, own main, assert-based, no framework / file I/O / wall clock; 14 `CHECK`s (spec asked ≥8), driven by simulated 128-frame device callbacks like `test_player_loop.cpp`; references are the engine's own `op::render_click`/`op::beat_onsets`, compared bit-exactly (`memcmp`/`==`, never a tolerance) |
| `CMakeLists.txt` | additive only: the exact §5 block after the WP-0200 block (`op_player_click_tests` / `add_test(NAME player_click …)`) and `list(APPEND DZ_OUR_TARGETS op_player_click_tests)  # WP-0201` right after the WP-0200 line |

Nothing else touched: `src/engine/*`, `src/cli/main.cpp`, `src/app/main.cpp`, `main_component.*`, `playback_controller.*`, `render_playback.*`, `docs/**`, `tests/test_player_loop.cpp`, `ROADMAP.md`, fixtures — all unchanged (`git status`/`git diff main --stat` below).

### Verification (commands + real output tails)

Builds/tests were run through the repo skill `dz-build` (`agent-build-kit/loop/tools/dz_build.sh`) — the mandated wrapper around `cmake -S/-B … && cmake --build … -j4` (shared lock, ccache); Release into `build/`, sanitizers via `DZ_BUILD_TYPE=Debug`.

1. **Release build, zero warnings:**
   ```
   $ bash agent-build-kit/…/dz_build.sh "$PWD" --out "$PWD/build"        # = cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4
   BUILD DIR: /home/shubhankar_dgx/qwen-scratch/openpattern/build          (exit 0)
   $ grep -icE "warning|error" build/dz_build.log
   0
   ```

2. **`./build/op_player_click_tests` → ALL PASS, exit 0:**
   ```
   PASS click.defaults_and_flag_round_trip
   PASS click.flag_off_output_is_plain_audio_bit_identical
   PASS click.failed_prepare_leaves_track_and_flag_untouched
   PASS click.reference_length_not_block_aligned
   PASS click.silent_audio_with_click_is_render_click_bit_for_bit
   PASS click.burst_starts_are_exactly_beat_onsets_with_0p5_onset_samples
   PASS click.mix_is_exactly_audio_plus_click_per_channel
   PASS click.flag_off_with_stored_track_is_plain_audio_and_counters_match
   PASS click.loop_window_bounds_land_mid_block
   PASS click.loop_every_slot_equals_reference_at_wrapped_frame
   PASS click.loop_bursts_refire_at_wrapped_beat_positions
   PASS click.window_shorter_than_block_wraps_click_in_every_slot
   PASS click.short_window_mono_reads_only_left_click_channel
   PASS click.no_click_where_nothing_was_copied_before_play_or_after_stop
   ALL PASS (0 failures)          # exit 0
   ```
   Coverage per §6: defaults/flag round-trip & prepare keeps flag off; failing `prepare_click` (`length_bars = 0.0`) → false + non-empty `err`, track+flag kept and still mixing; silent audio + click == `render_click` bit-for-bit incl. zero tail, burst starts exactly `beat_onsets(p)` with 0.5f onset samples and silent gaps; mix == audio+click per channel (and flag-off == plain audio, counters equal); loop wrap `[1000, 13000)` at 240 bpm (both bounds non-multiples of 128, 3 full rounds, every slot exact, onset burst re-fires ≥4× at the wrapped frame 12000); short window `[0, 97)` — wraps inside **every** block, all 2560 slots exact (beat-0 burst spans the whole window so any `pos+i`/running-counter indexing fails loudly), 27 onset hits, mono variant; zero blocks before `play()` and after `stop()` with track armed + flag on.

3. **Rule 9 regression — loop tests untouched:** `./build/op_player_loop_tests` →
   ```
   ALL PASS (0 failures)          # exit 0; file not in git diff (untouched)
   ```

4. **Unit tests / goldens / CLI / glue:**
   ```
   $ ./build/op_tests                                        → ALL PASS (0 failures)   # exit 0
   $ bash harness/run_goldens.sh                             → run_goldens: 15 fixtures, 0 failures
   $ bash harness/test_cli_errors.sh                         → test_cli_errors: 0 failures
   $ UV_CACHE_DIR=…/.cache/uv bash harness/test_glue_scripts.sh → test_glue_scripts: 0 failures
   ```

5. **Sanitizers** (`DZ_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`, i.e. `-g` not `-O3`; dir `build-san-wp0201` per the `build-san-wpXXXX` precedent of prior WPs):
   ```
   $ dz_build.sh "$PWD" --sanitize --out "$PWD/build-san-wp0201" --target op_player_click_tests --target op_tests   # exit 0
   $ ./build-san-wp0201/op_player_click_tests → ALL PASS (0 failures)   # exit 0, no ASAN/UBSAN/LSAN output at all
   $ ./build-san-wp0201/op_tests             → ALL PASS (0 failures)   # exit 0, clean
   ```

6. **§4 audio-callback purity grep — prints nothing:**
   ```
   $ grep -nE 'new|delete|malloc|calloc|realloc|free\(|std::vector|std::string|std::mutex|std::lock_guard|std::unique_lock|std::condition_variable|printf|std::cout|std::cerr|fopen|std::ofstream|std::ifstream|std::this_thread|sleep|push_back|resize|throw' src/app/player_rt.cpp
   rt grep exit: 1          # no matches, code or prose
   ```

7. **Changed-file set** (after the WP commit; `build*/` dirs are gitignored):
   ```
   $ git diff --name-only main
   CMakeLists.txt
   TASKS/WP-0201-dzplayer-metronome-during-playback.md
   src/app/player.cpp
   src/app/player.hpp
   src/app/player_rt.cpp
   tests/test_player_click.cpp
   ```

### Metrics

+125/−9 across the four modified files; new test file ≈ 430 lines, 14 checks. Click mixing costs one `load` of the flag + one compare per block plus ≤2 float adds per frame — no allocation, lock, clock or engine call in the callback.

### Notes / follow-ups

- First sanitizer attempt went through `dz_build.sh` at its default Release (`-O3`) + ASAN and hit a **pre-existing** GCC 13 `-Werror=array-bounds` false positive in `src/engine/render.cpp:754` (`out.arrangement = {clip}`, WP-0192 code, untouched by this WP — compiles clean at the specified Debug). Resolved by building sanitizers at Debug exactly as §Acceptance specifies; no engine change was made or needed.
- Untracked `wpdraft6/` in the checkout predates this WP (drafting-kit scratch); left untouched and uncommitted, so it never enters the diff above.
- No demo WAV attached: per §Out of scope, `opd render` / goldens never touch `dz::Player` and are unchanged (goldens 15/15 green is the regression proof); the audible change is live-playback-only and captured bit-exactly by the new test's comparisons against `op::render_click`.
- PROPOSE: none — nothing outside scope needed to change.
