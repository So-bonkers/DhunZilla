---
id: WP-0153
title: App playback — Play/Stop, background offline render, lock-free buffer swap, bar.beat position (WP-0137d)
phase: 1
status: ready
depends_on: [WP-0143]
estimate_agent_hours: 2
human_review: none
---

## Context — why this exists
WP-0137 (JUCE app skeleton) is split in parts. WP-0141 shipped `op::Document`, WP-0143 shipped the `dhunzilla`
window (transport bar with INERT Play/Stop, read-only track rows) and the headless `--snapshot` mode.
This part makes **Play and Stop work**: render the whole project offline with the existing engine on a
background thread, hand the finished buffer to the audio callback through an atomic pointer swap, and show
the position as `bar.beat`. The audio callback is a dumb executor (AGENTS.md real-time rules).
Because CI and the hidden check have no reliable audio device, everything that matters lives in a
**JUCE-free `Player`** that is also driven by a fake device in a deterministic headless mode,
`--render-playback`. The real device path is a thin wrapper around the same `Player::process`.
Read first: `docs/adr/0007-gui-toolkit-and-audio-backend.md` (48 kHz, 128 frames, fall back to default),
`AGENTS.md` (real-time rules), `src/app/main.cpp`, `src/app/main_component.{hpp,cpp}`, `src/engine/render.hpp`,
`src/engine/wav.hpp`, `src/doc/document.hpp` (`revision()`).

**Deferred (do NOT do here):** editing interactions / undo menu (part e), CI job `app`, seeking by clicking,
looping, a metering/xrun counter, resampling when the project rate is not the device rate (projects are
48 kHz; if they differ the app still plays the buffer — just wrong speed; not handled here).

## Spec — what "correct" means

### 1. Files (exactly these names)
| file | role |
|---|---|
| `src/app/player.hpp` | `dz::Player` declaration (JUCE-free, includes only `<atomic>`, `<memory>`, `engine/wav.hpp`) |
| `src/app/player.cpp` | control-thread methods: constructor, destructor, `submit`, `reclaim`, `play`, `stop`, `playing`, `position_frames`, `frames_played` |
| `src/app/player_rt.cpp` | **only** `Player::process` — THE AUDIO CALLBACK. |
| `src/app/render_playback.hpp/.cpp` | JUCE-free `dz::format_position` and `dz::run_render_playback` (below) |
| `src/app/playback_controller.hpp/.cpp` | JUCE glue `dz::PlaybackController`: audio device, background render, wiring |
| `src/app/main.cpp`, `src/app/main_component.{hpp,cpp}` | modified |
| `CMakeLists.txt`, `tests/test_main.cpp` | modified |

### 2. `dz::Player` (src/app/player.hpp) — exact public interface
```cpp
namespace dz {
class Player {
public:
    Player(); ~Player();                         // non-copyable
    // control thread (the app message thread / the headless driver) — never the audio thread
    void submit(std::unique_ptr<op::wav::StereoBuffer> buf);
    void reclaim();
    void play();
    void stop();
    bool playing() const;
    long long position_frames() const;   // 0 when a stop is pending, else the callback's sample counter
    long long frames_played() const;     // cumulative frames copied from a buffer by process()
    // audio thread
    void process(float* left, float* right, int num_frames) noexcept;  // right may be nullptr (mono)
};
}
```
**Buffer hand-off (atomic pointer swap, no invented lock-free structures):** two single-slot mailboxes of
`std::atomic<const op::wav::StereoBuffer*>`: `pending_` (control → audio) and `retired_` (audio → control),
plus a plain `current_` pointer owned by the audio thread. Rules:
- `submit(buf)`: `delete pending_.exchange(buf.release())` (a never-picked-up buffer is freed on the control
  thread), then `reclaim()`.
- `reclaim()`: `delete retired_.exchange(nullptr)`.
- `process()` start: **only if `retired_` is null**, `fresh = pending_.exchange(nullptr)`; if non-null:
  put the old `current_` (if any) into `retired_` and set `current_ = fresh`. (If `retired_` is occupied, adopt
  on the next block — never free or wait on the audio thread.) The old buffer is therefore always freed by
  `submit`/`reclaim`/`~Player` on the control thread, never in `process`.
- State: `std::atomic<bool> playing_`, `std::atomic<bool> reset_requested_`, `std::atomic<long long> position_`
  and `frames_played_` (both written only by `process`). `stop()` sets `playing_ = false` and
  `reset_requested_ = true`; `process()` applies `reset_requested_.exchange(false)` by zeroing `position_` at
  the start of a block; `position_frames()` returns 0 while `reset_requested_` is set.

**`process(left, right, n)` semantics (exact — the hidden check depends on them):**
1. Not playing, or no buffer adopted yet: write `n` zero frames; position unchanged.
2. Playing: `copied = min(n, frames - position)`; copy `copied` frames from the buffer's interleaved data
   at `position` into `left`/`right` (`right` skipped if null); the remaining `n - copied` frames are zeros;
   `position += copied`; `frames_played += copied`.
3. If `position >= frames` after the copy (end of song): `playing_ = false` and `position = 0`.
4. `play()` while already playing changes nothing. `play()` after `stop()` starts from frame 0.
   A buffer swap keeps the position (if the new buffer is shorter, rule 3 ends playback).

**`src/app/player_rt.cpp` must contain no `new`, `delete`, `malloc`, `calloc`, `realloc`, `free(`, `std::vector`,
`std::string`, `std::mutex`, `std::lock_guard`, `std::unique_lock`, `std::condition_variable`, `printf`,
`std::cout`, `std::cerr`, `fopen`, `std::ofstream`, `std::ifstream`, `std::this_thread`, `sleep`,
`push_back`, `resize`, or `throw`** (the check greps for these; only atomics, plain loops and
`<algorithm>` min/max are allowed). Put no comment text in it that contains those words either.

### 3. `src/app/render_playback.hpp/.cpp` (JUCE-free; library target `dzplayer`)
```cpp
namespace dz {
std::string format_position(const op::Project& p, long long frame);
int run_render_playback(const std::string& project_path, const std::string& out_path, int block,
                        long long stop_after, std::string& err);
}
```
**`format_position`** → `"<bar>.<beat>"`, both 1-based integers (`"1.1"`, `"2.3"`). `frame` is first clamped
to `[0, op::total_samples_for(p) - 1]`. `bar = 1 + b` where `b` is the largest integer in
`[0, ceil(length_bars) - 1]` with `op::sample_at_bar(p, b) <= frame`; `beat = 1 + k` where `k` is the largest
integer in `[0, beats_per_bar - 1]` with `op::sample_at_bar(p, b + k / (double)beats_per_bar) <= frame`.
(Use the engine's `sample_at_bar` — never re-implement tempo math. A binary search over `b` is fine.)
Examples at 120 bpm 4/4 48 kHz (96000 frames per bar): frame 0 → `1.1`; 23999 → `1.1`; 24000 → `1.2`;
95999 → `1.4`; 96000 → `2.1`; any frame past the end of a 4-bar project → `4.4`.

**`run_render_playback`** (the deterministic headless path; same `Player` code as the real device):
1. `project_path` must exist (else `err = "project not found: <path>"`, return 3); load with `op::load_project`
   (failure: return 3, `err` from the loader); render with `op::render_project`, `base_dir` = the project file's
   parent directory (exactly like `opd render`; failure: return 5).
2. Create a `Player`, `submit` the rendered buffer, `play()`.
3. Fake device: loop `player.process(left, right, block)` with float arrays of `block` frames, and append the
   produced frames to an output `StereoBuffer` (same sample rate), keeping exactly the first `total` frames
   (`total` = rendered frame count; the last block's tail is dropped, i.e. `ceil(total/block)` blocks).
4. Write with `op::wav::write_pcm16_stereo(out_path, ...)` (return 5 on failure).
5. `stop_after < 0`: one run as above. `stop_after >= 1`: let `seg = min(total, ceil(stop_after/block)*block)`.
   Run blocks until `seg` frames are produced (the last of these blocks contributes only what is needed),
   read `before = player.position_frames()`, call `player.stop()`, run ONE more block (not written) and check it
   is entirely zeros and `position_frames() == 0 && !playing()`, call `player.play()` and run the full
   `total` frames again from the start. The WAV is then `seg` frames followed by `total` frames.
6. Stdout on success, exactly:
   `render-playback: <frames> frames @<rate> Hz block=<block> blocks=<blocks> -> <out_path>` where `<frames>` is
   the number of frames written and `<blocks>` is the number of `process()` calls made (including the silent one);
   and, only when `stop_after >= 1`, a second line
   `stop-after: segment=<seg> frames position_before_stop=<before> position_after_stop=<p> silent_after_stop=<0|1>`
   (`silent_after_stop=1` iff the silent block was all zeros and `position_after_stop == 0`).

**Equality guarantee (stated exactly):** for any block size `B >= 1`, the WAV written by
`dhunzilla --render-playback out.wav --block B project.opj` is **byte-identical** (`cmp` exit 0) to
`opd render project.opj -o ref.wav` — canonical 44-byte-header PCM16 stereo WAV at the project sample
rate, written by the same `write_pcm16_stereo`, with the same frames. With `--stop-after N`, the WAV equals
the first `seg` frames of that render followed by the whole render.

### 4. CMake
- Add **unconditionally** (default build, no JUCE): `add_library(dzplayer STATIC src/app/player.cpp
  src/app/player_rt.cpp src/app/render_playback.cpp)`, `target_include_directories(dzplayer PUBLIC src/app)`,
  `target_link_libraries(dzplayer PUBLIC opengine)`; link `dzplayer` into `op_tests`; add `dzplayer` to
  `DZ_OUR_TARGETS` (so it gets `-Werror` and the sanitizer flags like the other core libs).
- Inside `if(DZ_BUILD_APP)`: add `src/app/playback_controller.cpp` to the `dhunzilla` sources; link
  `dzplayer` and `juce::juce_audio_devices` (in addition to `juce::juce_gui_basics`); extend the existing
  per-source `-Werror` property list to include `src/app/playback_controller.cpp`.
- The default configure must still never mention JUCE and create no `_deps`.

### 5. Controller + UI (JUCE glue; src/app/playback_controller.*, main_component.*, main.cpp)
- `dz::PlaybackController` owns a `dz::Player`, a `juce::AudioDeviceManager`, a `juce::ThreadPool` with 1
  thread, and implements `juce::AudioIODeviceCallback` whose output callback does nothing but call
  `player.process(outputs[0], numOutputs >= 2 ? outputs[1] : nullptr, numSamples)` and zero any channels
  beyond 2 (no allocation, locks or I/O).
- `bool openDevice(std::string& err)`: `initialiseWithDefaultDevices(0, 2)`, then request 48000 Hz and a
  128-frame buffer via `setAudioDeviceSetup` (prefer an output device whose name contains `PipeWire`); if that
  returns an error, fall back to the default device settings; fail only if no output device opens.
  `deviceDescription()` returns e.g. `<device name> 48000 Hz, 128 frames` (actual values) or `no audio device`.
- `requestRender(const op::Document&)` (message thread): take a COPY of `doc.project()`, add a pool job that
  calls `op::render_project(copy, base_dir)` (base_dir = parent dir of `doc.path()`), and on completion
  `juce::MessageManager::callAsync` → if still the latest request (monotonic request id) `player.submit(...)`;
  stale results are dropped, errors are kept as `lastError()`. An `alive` flag (shared `std::atomic<bool>`)
  guards the async lambda against destruction; the destructor removes pool jobs, waits for a running one, removes
  the audio callback and closes the device before the `Player` dies.
- `play(doc)` = `player.play()` + `requestRender(doc)`; `stop()` = `player.stop()`.
- `MainComponent(const op::Document&, PlaybackController* playback = nullptr)`: with `nullptr` (snapshot mode)
  behaviour and pixels stay as in WP-0143 (Play/Stop disabled, no timer, empty status line, position `1.1`).
  With a controller: Play/Stop enabled and wired (`onClick`); a 30 Hz `juce::Timer` calls `player.reclaim()`,
  re-renders when `player.playing()` and `doc.revision()` changed since the last request (this is the "after any
  model change while playing" path), updates the position label with `format_position(doc.project(),
  player.position_frames())`, and shows `deviceDescription()` (or `lastError()`) in a new bottom status label
  (`getHeight() - 28`, height 24, left margin 16). `refresh()` also formats the position this way
  (`1.1` when stopped).
- `main.cpp`: window mode creates the controller, calls `openDevice` (on failure print one line
  `dhunzilla: audio: <reason>` to stderr and continue without sound), destroys window then controller then
  document on shutdown. **`--snapshot` must never open a device or create a controller.**

### 6. Command line (all exit codes; stderr gets exactly one line `dhunzilla: <reason>` on failure)
New options (any order, alongside the WP-0143 ones): `--render-playback <out.wav>`, `--block <N>` (integer
1..8192, default 128), `--stop-after <N>` (integer >= 1), `--position-at <FRAME>` (integer >= 0),
`--play-seconds <N>` (integer >= 1). Exit 2 (usage, checked before touching the filesystem) for: a missing or
non-integer/out-of-range value; more than one of `--snapshot` / `--render-playback` / `--position-at` /
`--play-seconds`; any of those without a project path; `--block` or `--stop-after` without `--render-playback`.
- `dhunzilla --render-playback out.wav [--block B] [--stop-after N] project.opj` → section 3. Exit 0; 3 project
  missing/invalid; 5 render or write error (nothing printed to stdout on failure).
- `dhunzilla --position-at FRAME project.opj` → prints exactly `position: <bar>.<beat>` (`format_position`)
  and exits 0 (3 if the project is bad).
- `dhunzilla --play-seconds N project.opj` (local smoke, opens the REAL device, never run by CI/the check):
  plays until `frames_played >= N * 48000` or the song ends, prints exactly
  `played: <frames> frames, device=<name> sr=<sr> buf=<n>`, exits 0; exit 6 (`dhunzilla: audio: <reason>`)
  if no device opens or the callback produced no frames within N + 40 s.
These modes still need an X display (JUCE application startup), exactly like `--snapshot`.

### 7. Unit tests (`tests/test_main.cpp`, op_tests stays JUCE-free)
Add a `test_player()` (called from `main`) with **at least 8 new `CHECK`s** using `dz::Player` directly
(include `"player.hpp"` and `"render_playback.hpp"`): silence without a buffer; silence when not playing;
copy + position advance; end-of-song (partial block then zeros, `playing()` false, position 0); stop resets
position and silences; restart from zero; buffer swap keeps position; `right == nullptr` works;
`format_position` of frame 0 is `"1.1"`.

## Acceptance criteria
- [x] Default build: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4` — no JUCE
      mention, no `_deps`, no warnings; `op_tests` passes (≥ 8 new CHECKs).
- [x] App build per the recipe in `CMakeLists.txt` comments (local JUCE source via
      `-DFETCHCONTENT_SOURCE_DIR_JUCE`), `-j4`, in the flock; zero warnings from `src/app/*`.
- [x] `<app> --render-playback /tmp/p.wav --block 128 tests/fixtures/goldens/demo.opj` → exit 0, and
      `cmp /tmp/p.wav <opd render of the same project>` is identical for blocks 64, 128, 512 and 1000 (also try 1
      on a short project if it is fast enough) and for at least `demo.opj`, `tempo.opj`, `mixer.opj`, `arranged.opj`.
- [x] `--stop-after 30000 --block 128` produces `ceil(30000/128)*128 = 30080` frames then the full render,
      and the second stdout line has `position_before_stop=30080 position_after_stop=0 silent_after_stop=1`.
- [x] `--position-at` prints `1.1`, `1.2`, `2.1` for frames 0, 24000, 96000 on `demo.opj`.
- [x] `grep -nE` for the forbidden tokens in `src/app/player_rt.cpp` finds nothing.
- [x] `--snapshot` still works unchanged (`--snapshot /tmp/a.png demo.opj` → PNG 1100x700) and opens no device.
- [x] Local smoke if this machine has audio: `<app> --play-seconds 2 demo.opj` prints the `played:` line (put
      the real output in the handoff; if there is no device, say so — it is not required).
- [x] Standard gates on the default build: `./build/op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`; sanitizer build
      (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`) `op_tests` passes.

## Out of scope — do NOT touch
- `src/engine/`, `src/cli/`, `src/doc/`, `docs/adr/`, `docs/spec/`, `spikes/`, `ai-sidecar/`, `.github/`,
  golden fixtures (`tests/fixtures/`), `harness/`. **Do not edit `ROADMAP.md`** (the elephant adds the row).
- Editing interactions, undo/redo UI, CI, seeking, loop, metering.
- Do not commit build directories or generated WAV/PNG files.

## Git
- Branch `wp-0153-app-playback` from `main`. Commit: `WP-0153: App playback`.
  Stage explicit paths only (`CMakeLists.txt`, `src/app/*`, `tests/test_main.cpp`, this task file); never
  `git add -A` / `git add .`.
- **Do not push or merge.** Build with `-j4` at most; wrap every JUCE build in
  `flock <workspace>/agent-build-kit/.calib.lock <command>` if that file exists.

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/app/player.hpp` / `player.cpp` / `player_rt.cpp` (new): JUCE-free `dz::Player` — two single-slot
    atomic pointer mailboxes (`pending_`, `retired_`) + audio-thread-owned `current_`; buffer destruction
    only ever happens on the control thread (`submit`/`reclaim`/`~Player`). `player_rt.cpp` holds **only**
    `process()`: atomics, plain loops, `<algorithm>` min — verified token-clean (see evidence).
  - `src/app/render_playback.hpp/.cpp` (new): `dz::format_position` (bar.beat via the engine's own
    `sample_at_bar`, binary search over bars, never re-derived) and `dz::run_render_playback`
    (load → render → fake-device block loop through the same `Player::process` → PCM16 WAV; `--stop-after`
    segment/stop/silent-block/restart exercise).
  - `src/app/playback_controller.hpp/.cpp` (new): JUCE glue — `AudioDeviceManager` (prefer PipeWire-named
    output, request 48 kHz/128 frames, fall back to defaults), 1-thread `ThreadPool` offline render with
    monotonic request-id staleness drop + shared `alive` flag guard; audio callback only calls
    `player.process(out[0], out[1]?…, n)` and zeroes channels ≥ 2. Destructor: alive→false, drain pool,
    remove callback, close device — before the Player dies.
  - `src/app/main_component.{hpp,cpp}`: optional `PlaybackController*`; null (snapshot) keeps WP-0143 pixels
    exactly (buttons disabled, no timer, empty status line, position "1.1"); with a controller: wired
    Play/Stop, 30 Hz timer (`reclaim`, re-render on revision change while playing, `format_position` label,
    device/error bottom status line at `getHeight()-28`, h=24, left 16). `refresh()` formats position too.
  - `src/app/main.cpp`: new options `--render-playback/--block/--stop-after/--position-at/--play-seconds`
    (grammar fully checked before any filesystem access), the three headless modes, window mode with
    controller (audio failure = one stderr line, app continues), shutdown order window→controller→document.
    `--snapshot` never creates a controller or opens a device.
  - `CMakeLists.txt`: unconditional `dzplayer` static lib (added to `DZ_OUR_TARGETS`, linked into `op_tests`);
    inside `DZ_BUILD_APP`: `playback_controller.cpp` source, link `dzplayer` + `juce::juce_audio_devices`,
    `-Werror` property extended.
  - `tests/test_main.cpp`: `test_player()` — **12 new CHECKs** (silence w/o buffer; silence not playing;
    copy+advance; swap keeps position; stop resets+silences; restart from zero; end-of-song partial→zeros;
    right==nullptr; format_position home/beat-boundary/bar-boundary/past-end-clamp).
  - One bug found and fixed by the new tests mid-WP: bar binary-search upper bound was off-by-one
    (`format_position(96000)` returned `1.4`); rewritten with explicit inclusive bounds.

- **How verified:** (commands + actual output)
  1. Default build, no JUCE, no `_deps`, zero warnings:
     ```
     $ cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4   # exit 0
     $ cmake -B <fresh> 2>&1 | grep -i juce ; echo $?      → (no output) 1 = no JUCE mention in configure output
     $ ls build/_deps                                       → No such file or directory
     ```
  2. `op_tests` — 12 new CHECKs, whole suite green:
     ```
     PASS player.silence_without_buffer
     PASS player.silence_when_not_playing
     PASS player.copy_and_position_advance
     PASS player.swap_keeps_position
     PASS player.stop_resets_position_and_silences
     PASS player.restart_from_zero
     PASS player.end_of_song_partial_then_zeros
     PASS player.right_null_mono_output
     PASS format_position.frame_0_is_1.1
     PASS format_position.beat_boundary
     PASS format_position.bar_boundary
     PASS format_position.clamps_past_end
     ALL PASS (0 failures)
     ```
  3. App build per the CMakeLists recipe (`-DFETCHCONTENT_SOURCE_DIR_JUCE=<local JUCE 9.0.5>`, under
     `flock …/agent-build-kit/.calib.lock`, `-j4`): links, **zero warnings from src/app/*** —
     `grep -c "src/app.*warning" buildlog` → `0`.
  4. Byte-identity vs `opd render` (blocks 64/128/512/1000 × demo/tempo/mixer/arranged, plus block=1 on
     demo): all 17 `cmp` runs identical:
     ```
     IDENTICAL demo block=64 … IDENTICAL arranged block=1000   (16/16)
     render-playback: 192000 frames @48000 Hz block=1 blocks=192000 -> .../rp_demo_1.wav
     IDENTICAL demo block=1
     ```
  5. `--stop-after 30000 --block 128` (demo.opj, total=192000 frames):
     ```
     render-playback: 222080 frames @48000 Hz block=128 blocks=1736 -> .../rp_stop.wav
     stop-after: segment=30080 frames position_before_stop=30080 position_after_stop=0 silent_after_stop=1
     ```
     and the WAV layout verified by dd/cmp: first 30080 frames == head of `ref_demo.wav`, remainder == whole render.
  6. `--position-at` on demo.opj → `position: 1.1` / `position: 1.2` / `position: 2.1` for 0/24000/96000.
  7. Forbidden-token grep on `src/app/player_rt.cpp`:
     ```
     $ grep -nE 'new|delete|malloc|calloc|realloc|free\(|std::vector|std::string|std::mutex|…|throw' src/app/player_rt.cpp
     $ echo $?   → 1 (no matches; word-boundary-free pattern, so also no comment prose hits)
     ```
  8. `--snapshot` unchanged: exit 0, stdout `title: DhunZilla — demo.opj` + `snapshot: …/a.png 1100x700`;
     PNG header parsed → `PNG size: 1100 x 700`. Snapshot path never constructs a controller (statically;
     the mode returns before any PlaybackController exists).
  9. Usage/exit contract (all want==got): missing/non-integer/out-of-range option values → 2;
     `--snapshot`+`--render-playback` together → 2; mode flag without project → 2; `--block`/`--stop-after`
     without `--render-playback` → 2; missing project file → 3 (`dhunzilla: project not found: …`);
     invalid project → 3 (loader error). Stdout on failure verified empty (`stdout on failure: ''`).
  10. Standard gates on the default build:
      `bash harness/run_goldens.sh` → `run_goldens: 9 fixtures, 0 failures`;
      `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`;
      `UV_CACHE_DIR=… bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`;
      sanitizer build (`-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`, dzplayer included in DZ_OUR_TARGETS)
      → `ALL PASS (0 failures)` under ASAN+UBSAN (no leaks reported at exit).
  11. Local `--play-seconds` smoke **ran on this machine** (PipeWire's ALSA sink is present despite no
      `/dev/snd` in the container):
      ```
      $ ./build-app/dhunzilla_artefacts/Release/dhunzilla --play-seconds 2 tests/fixtures/goldens/demo.opj
      played: 96000 frames, device=Default ALSA Output (currently PipeWire Media Server) sr=48000 buf=128
      exit 0
      ```

- **Follow-ups noticed (not done):**
  - Headless modes (`--position-at`, `--render-playback`) load via `op::load_project` while window mode uses
    `Document::open` — intentional (no Document needed headless), but a future CLI consolidation could share
    one loader entry point.
  - The fake-device loop drops the last block's tail; with `--stop-after`, `position_before_stop` reflects the
    player's full-block position (30080 here), which is what the spec asks to print — noted for part-e authors.
  - JUCE 9.0.5 upstream itself emits one `-Wdangling-reference` warning in `juce_LookAndFeel_V2.cpp` during
    app builds (pre-existing, not from `src/app/*`; out of scope per WP-0143's "upstream warnings are not ours").

- **PROPOSE / BLOCKED:** none.
