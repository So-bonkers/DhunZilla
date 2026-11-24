---
id: WP-0241
title: Transport block-size sweep - dz::Player over every golden project must be byte-identical to `opd render` (test-only)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, new:tests/test_player_blocksweep.cpp]
---

## Context - why this exists

Phase 1 playback is one sentence: render the project offline, hand the buffer to `dz::Player`, and let the
audio callback copy frames out of it. Every transport feature added since (loop region WP-0200, metronome
WP-0201, and later latency compensation or per-block processing) can quietly change what the *live* path
outputs while `opd render` stays untouched - and nothing today drives the callback with anything but fixed
128-frame blocks over a synthetic ramp. This package adds ONE test that pins the invariant over the whole
golden corpus: play every `tests/fixtures/goldens/*.opj` through `dz::Player::process()` driven by a
fixed-seed sequence of callback sizes (32..512 samples at the 48 kHz golden rate), concatenate exactly the
project's frames, write them as PCM16 WAV and require the file bytes to equal those of
`opd render <project> -o ...`. It adds no engine, CLI, app or spec code and changes no behaviour; it is a
regression net for future transport work.

Existing code this package reuses (read it first, do not reimplement or modify it): `dz::Player` in
`src/app/player.hpp` (library target `dzplayer`, callback in `src/app/player_rt.cpp`); the fake-device
driving idiom of `dz::run_render_playback` in `src/app/render_playback.cpp` (it keeps exactly `total`
frames and drops the tail of the stopping block); `op::load_project` (`src/engine/model.hpp`),
`op::render_project` (`src/engine/render.hpp`), `op::wav::StereoBuffer` / `op::wav::write_pcm16_stereo`
(`src/engine/wav.hpp`); the `OPD_BIN` + `popen` + `WEXITSTATUS` idiom of `tests/test_tracks.cpp`; the
golden corpus rule of `harness/run_goldens.sh` (every `*.opj` directly in `tests/fixtures/goldens`).

## Spec - normative (this task file is the contract)

### Files (exactly these 2; stage no others)
- `tests/test_player_blocksweep.cpp` (new) - the whole test, with its own `main` and its own
  `CHECK(cond, name)` macro printing `PASS <name>` / `FAIL <name>`; includes `engine/model.hpp`,
  `engine/render.hpp`, `engine/wav.hpp` and `player.hpp` (the `dzplayer` target already exports
  `src/app`), plus `<random>` for `std::mt19937`; unique scratch dir
  (`std::filesystem::temp_directory_path()` + name containing `::getpid()`), never a fixed `/tmp/op_*`
  path. Do NOT edit `tests/test_main.cpp` and do not add a second test file.
- `CMakeLists.txt` - purely additive, exactly these lines inserted between the existing line
  `add_test(NAME tracks COMMAND op_tracks_tests)` and the line starting `# Our targets only`:

      # WP-0241: live-vs-render transport sweep - dz::Player driven by fixed-seed 32..512-sample callbacks over
      # every golden project, byte-compared with `opd render`. Needs the dzplayer core, the fixtures and opd.
      add_executable(op_player_blocksweep_tests tests/test_player_blocksweep.cpp)
      target_link_libraries(op_player_blocksweep_tests PRIVATE opengine dzplayer)
      target_compile_definitions(op_player_blocksweep_tests PRIVATE OP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests"
          OPD_BIN="$<TARGET_FILE:opd>")
      add_dependencies(op_player_blocksweep_tests opd)
      add_test(NAME player_blocksweep COMMAND op_player_blocksweep_tests)

  and exactly this one line immediately after `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227`:

      list(APPEND DZ_OUR_TARGETS op_player_blocksweep_tests)  # WP-0241

  No other CMake line, no new source file, no new library target.

### The procedure (normative, per project)

Let `<goldens>` be the directory `tests/fixtures/goldens` (use the `OP_TEST_DATA` macro: it is
`<repo>/tests`, so the corpus is `OP_TEST_DATA "/fixtures/goldens"`). The corpus is every *regular file*
directly in it whose extension is `.opj`, sorted by full path, not recursive - the same set
`harness/run_goldens.sh` iterates (16 projects at this base). `<stem>` is the file name without `.opj`.
For each project, in that order:

1. Reference - run the real CLI binary: `"<OPD_BIN>" render "<project.opj>" -o "<scratch>/<stem>.ref.wav"`
   (`popen`, exit code from `WEXITSTATUS(pclose(...))`; exit code must be 0). Its bytes are the reference.
2. Live path - `op::load_project`, then `op::render_project(p, base_dir, buf, err)` with
   `base_dir` = the project file's parent directory (sampler path resolution, exactly as `opd render` does).
   Submit a copy of that buffer to a fresh `dz::Player` (`player.submit(std::make_unique<op::wav::StereoBuffer>(buf))`)
   and `player.play()`. No loop region, no `prepare_click`, no `stop()`: plain playback.
3. Fake device - call `player.process(left.data(), right.data(), block)` with `block` drawn from ONE
   fixed-seed sequence: `std::mt19937` constructed fresh per project with the seed literal `20240241u`, each
   draw `kMinBlock + static_cast<int>(rng() % static_cast<unsigned>(kMaxBlock - kMinBlock + 1))` with
   `kMinBlock = 32` and `kMaxBlock = 512` (both inclusive; the goldens are 48 kHz, so the biggest callback
   is ~10.7 ms). Keep calling until every frame is produced.
4. Concatenation - keep exactly `buf.frames()` frames: from each callback take
   `std::min(static_cast<long long>(block), total - produced)` leading frames and drop the rest of the block
   (the player zero-fills the past-the-end tail and parks at frame 0; that tail is not part of the output).
5. Compare - write the concatenated buffer with `op::wav::write_pcm16_stereo` to
   `<scratch>/<stem>.live.wav` and require the two files' bytes to be equal, whole file including the
   44-byte header (`size()` equal and `std::memcmp` == 0). No tolerance, no epsilon, no "ignore the tail",
   no per-channel or per-frame comparison.

One comparison helper (same size + `std::memcmp`) is used for every project check, and the two
negative-control CHECKs below exercise exactly that helper, so a comparison that cannot fail is impossible
to hide.

### Output the acceptance check parses (fix these strings exactly)

Per project, one line on stdout in corpus order:

    SWEEP <stem> frames=<F> blocks=<B> min_block=<M> max_block=<X> bytes=<N> match=byte-identical

`<F>` = frames of the render, `<B>` = the number of `process()` calls it took, `<M>`/`<X>` = smallest /
largest block drawn for that project, `<N>` = the live file size in bytes, `match=mismatch` instead of
`match=byte-identical` when the bytes differ. That line is printed for every project - failed ones included,
with `match=mismatch` - and carries nothing before `SWEEP` and nothing after the `match=` value. The last
stdout line is the repo convention: `ALL PASS (0 failures)`, or `<k> FAILURE(S)`; the exit code is the
number of failures.

### Required CHECKs (names verbatim; the hidden check greps these `PASS` lines)

- one per corpus project: `blocksweep.<stem>.matches_opd_render` - true iff that project's live WAV bytes
  equal the `opd render` bytes (and the project's render, submit and write all succeeded).
- `blocksweep.every_golden_project_swept` - the number of swept projects equals the corpus size, and the
  corpus size is >= 10.
- `blocksweep.every_project_is_byte_identical` - every project's comparison passed.
- `blocksweep.callback_counts_match_the_frame_totals` - for every project
  `(F + 511) / 512 <= B <= (F + 31) / 32` (integer arithmetic): the count is consistent with 32..512-sample
  callbacks producing exactly F frames.
- `blocksweep.references_are_canonical_pcm16_stereo_wavs` - for every project the reference file is
  `44 + 4 * F` bytes and the live file has the same size.
- `blocksweep.transport_counters_match_the_played_frames` - after each run `!player.playing()` and
  `player.frames_played() == F`.
- `blocksweep.block_sizes_stay_in_range_and_actually_vary` - over all projects: every drawn block is in
  `[32, 512]`, the smallest drawn block is `<= 64` and the largest is `>= 448`. A single-size sweep (the
  old 128-frame habit) must fail this CHECK.
- `blocksweep.two_fixed_seed_sweeps_are_bit_identical` - sweep the first corpus project a second time and
  require its live bytes to equal the first run's bytes (the sequence is fixed-seed, so the whole run is
  reproducible).
- `blocksweep.negative_control_bit_flip_detected` - take the live bytes of a swept project, flip one byte
  at offset `44 + 4 * 100` (`^= 0xFF`), and require the comparison helper to report *different* from the
  reference.
- `blocksweep.negative_control_block_swap_detected` - swap two 128-frame chunks of the live bytes (byte
  ranges starting at `44 + 4 * 1000` and `44 + 4 * 5000`) and require the helper to report *different*.
That is >= 24 CHECKs with the 16 goldens at this base. A test that only covers a hand-picked subset of the
corpus is not this package.

### If a project mismatches

This package is test-only. Do NOT touch the engine, `src/app/`, the CLI, or the goldens to make a mismatch
pass, and never edit anything under `tests/fixtures/`. If `opd render` and the transport genuinely differ
for some golden, leave the test honest (the package then fails its gates), and write a `PROPOSE:` note at
the bottom of the task file naming the project, `<F>`, `<B>`, the byte offset of the first difference and
the callback number where it first shows up. That note - not a fix - is the deliverable for a mismatch.

### Determinism / repo rules (ADR-0004)

No wall clock (`std::chrono` forbidden in the test), no `std::random_device`, no time-derived seed: the only
randomness source is `std::mt19937` with the seed literal `20240241u`, so two runs of the binary print
identical `SWEEP` lines. Fixed 48 kHz from the fixtures; no fast-math; the file must compile with
`-Wall -Wextra -Werror` and run clean under `-DDZ_SANITIZE=ON` (ASAN+UBSAN).

## Acceptance criteria

- [ ] `tests/test_player_blocksweep.cpp` + the additive `CMakeLists.txt` lines above; `git diff --name-only`
      shows exactly those two paths.
- [ ] The built binary prints one `SWEEP <stem> ...` line per `tests/fixtures/goldens/*.opj` (16 at this
      base) and exits 0; every project reports `match=byte-identical`.
- [ ] All required CHECK names above appear as `PASS` lines; at least 24 CHECKs in total.
- [ ] Two consecutive runs of the binary print byte-identical `SWEEP` lines (fixed seed).
- [ ] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_player_blocksweep_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, and the
      `-DDZ_SANITIZE=ON` build of `op_tests` and `op_player_blocksweep_tests`.
- [ ] No behaviour change: every pre-existing gate passes exactly as before.

## Out of scope - do NOT touch

- `src/` (engine, cli, app, doc), `dz::Player` / `player_rt.cpp` included: no engine or callback change.
- `tests/fixtures/` (goldens are read-only inputs), `harness/`, `docs/` (no observable behaviour changes,
  so no spec edit), `ROADMAP.md`, `.github/`, `ai-sidecar/`, `tools/`, `demos/`, `spikes/`,
  `tests/test_main.cpp`.
- No new `opd` subcommand, no new `dz_*` tool, no `--block`-style flag, no change to
  `dz::run_render_playback`, no new golden, no CI change. This package is a test and CMake registration only.

## Git
- Branch `wp-0241-dzplayer-vs-opd-render` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0241: transport block-size sweep - dz::Player byte-identical to opd render on every golden`. Stage explicit paths only (never `git add -A`).
  Include `TASKS/WP-0241-dzplayer-vs-opd-render.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

Branch `wp-0241-dzplayer-vs-opd-render`, base `d7e3fa3` (the integration base; main was not switched to).
Everything below was run on this branch on the aarch64 DGX Spark rig; command paths are shortened relative to
`/home/shubhankar_dgx/qwen-scratch` (`openpattern/` = the checkout, `builds/wp0241*` = dz-build output dirs kept
outside the checkout), all outputs are verbatim. No `PROPOSE:` blocker for the transport; the package is test-only
and every gate is green.

### What changed

- `tests/test_player_blocksweep.cpp` (new, 341 lines) — the whole test: own `main`, own `CHECK(cond, name)`,
  `bytes_are_identical()` as the single whole-file comparison helper (same size + `std::memcmp`), corpus from
  `OP_TEST_DATA "/fixtures/goldens"` (regular `.opj` files, sorted by full path, not recursive), per project
  `popen` of `"<OPD_BIN>" render <opj> -o <ref.wav>` with `WEXITSTATUS(pclose(...))`, in-process
  `op::load_project` + `op::render_project(p, <project dir>, buf, err)`, `player.submit(std::make_unique<…>(buf))`
  + `player.play()` (no loop, no click, no stop), one fresh `std::mt19937(20240241u)` per project drawing
  `32 + rng() % 481`, keeping exactly `buf.frames()` frames, `op::wav::write_pcm16_stereo`, whole-file compare.
  Scratch dir `temp_directory_path() / "op_player_blocksweep_wp0241_" + getpid()`, removed at both ends.
  No `std::chrono`, no `std::random_device`, no time-derived seed.
- `CMakeLists.txt` — +10 lines, purely additive: exactly the specified registration block between
  `add_test(NAME tracks COMMAND op_tracks_tests)` and `# Our targets only …`, plus exactly the one
  `list(APPEND DZ_OUR_TARGETS op_player_blocksweep_tests)  # WP-0241` line after the WP-0227 line. No new
  library target, no other CMake line.
- Nothing else. `src/` (engine, cli, app), `harness/`, `docs/`, `tests/fixtures/`, `tests/test_main.cpp`
  untouched; no behaviour, spec, CLI or CI change.

### Acceptance criteria — with evidence

**[x] Exactly the two code paths (plus the task file the Git section requires).**

```
$ git add CMakeLists.txt tests/test_player_blocksweep.cpp TASKS/WP-0241-dzplayer-vs-opd-render.md
$ git diff --cached --name-only
CMakeLists.txt
TASKS/WP-0241-dzplayer-vs-opd-render.md
tests/test_player_blocksweep.cpp
$ git diff --cached --stat -- CMakeLists.txt tests/test_player_blocksweep.cpp
 CMakeLists.txt                   |  10 ++
 tests/test_player_blocksweep.cpp | 341 +++++++++++++++++++++++++++++++++++++++
 2 files changed, 351 insertions(+)
```

The complete CMake diff (nothing else in that file changed):

```
@@ -127,6 +127,15 @@ target_compile_definitions(op_tracks_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
 add_dependencies(op_tracks_tests opd)
 add_test(NAME tracks COMMAND op_tracks_tests)

+# WP-0241: live-vs-render transport sweep - dz::Player driven by fixed-seed 32..512-sample callbacks over
+# every golden project, byte-compared with `opd render`. Needs the dzplayer core, the fixtures and opd.
+add_executable(op_player_blocksweep_tests tests/test_player_blocksweep.cpp)
+target_link_libraries(op_player_blocksweep_tests PRIVATE opengine dzplayer)
+target_compile_definitions(op_player_blocksweep_tests PRIVATE OP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests"
+    OPD_BIN="$<TARGET_FILE:opd>")
+add_dependencies(op_player_blocksweep_tests opd)
+add_test(NAME player_blocksweep COMMAND op_player_blocksweep_tests)
+
 # Our targets only — list explicitly so third_party/ can never trip these gates.
@@ -148,6 +157,7 @@ list(APPEND DZ_OUR_TARGETS op_player_click_tests)  # WP-0201
 list(APPEND DZ_OUR_TARGETS op_audition_tests)  # WP-0209
 list(APPEND DZ_OUR_TARGETS op_clip_trim_midi_tests)  # WP-0193c
 list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227
+list(APPEND DZ_OUR_TARGETS op_player_blocksweep_tests)  # WP-0241
```

**[x] One `SWEEP <stem> …` line per golden (16), exit 0, every project `match=byte-identical`.**

```
$ B=$(bash agent-build-kit/loop/tools/dz_build.sh openpattern --out builds/wp0241 \
        --target op_player_blocksweep_tests --target opd --target op_tests | tail -1)
$ echo "$B"
/home/shubhankar_dgx/qwen-scratch/builds/wp0241
$ "$B/op_player_blocksweep_tests"; echo "EXIT=$?"
SWEEP arranged frames=480000 blocks=1759 min_block=32 max_block=512 bytes=1920044 match=byte-identical
SWEEP clip_fades frames=384000 blocks=1412 min_block=32 max_block=512 bytes=1536044 match=byte-identical
SWEEP clip_trim frames=384000 blocks=1412 min_block=32 max_block=512 bytes=1536044 match=byte-identical
SWEEP demo frames=192000 blocks=706 min_block=32 max_block=509 bytes=768044 match=byte-identical
SWEEP imported frames=96000 blocks=356 min_block=32 max_block=509 bytes=384044 match=byte-identical
SWEEP mixer frames=384000 blocks=1412 min_block=32 max_block=512 bytes=1536044 match=byte-identical
SWEEP panorama frames=128000 blocks=470 min_block=32 max_block=509 bytes=512044 match=byte-identical
SWEEP pitched frames=192000 blocks=706 min_block=32 max_block=509 bytes=768044 match=byte-identical
SWEEP resampled frames=96000 blocks=356 min_block=32 max_block=509 bytes=384044 match=byte-identical
SWEEP sampler_choke frames=96000 blocks=356 min_block=32 max_block=509 bytes=384044 match=byte-identical
SWEEP sampler_repitch frames=96000 blocks=356 min_block=32 max_block=509 bytes=384044 match=byte-identical
SWEEP sampler_trim frames=96000 blocks=356 min_block=32 max_block=509 bytes=384044 match=byte-identical
SWEEP synth_v1 frames=192000 blocks=706 min_block=32 max_block=509 bytes=768044 match=byte-identical
SWEEP tempo frames=1024000 blocks=3735 min_block=32 max_block=512 bytes=4096044 match=byte-identical
SWEEP time_sig_3_4 frames=216000 blocks=793 min_block=32 max_block=509 bytes=864044 match=byte-identical
SWEEP time_sig_6_8 frames=144000 blocks=533 min_block=32 max_block=509 bytes=576044 match=byte-identical
…(PASS lines, next section)…
ALL PASS (0 failures)
EXIT=0
$ "$B/op_player_blocksweep_tests" | grep -c '^SWEEP '   # corpus lines
16
```

Corpus = 16 `.opj` (`ls tests/fixtures/goldens/*.opj | wc -l` → `16`); every `<F>` equals the committed golden
frame count, every `<N>` equals the golden `.wav` size (44 + 4·F). Mismatch diagnostics (if any project ever
mismatches) go to **stderr** only, so stdout stays exactly SWEEP lines + PASS/FAIL lines + summary; nothing was
written to stderr on this run (`stat -c %s /tmp/run1.err` → `0`).

**[x] Every required CHECK name appears as a `PASS` line; 25 CHECKs (>= 24).**

```
$ "$B/op_player_blocksweep_tests" | grep -c '^PASS'
25
$ "$B/op_player_blocksweep_tests" | grep -c '^FAIL'
0
```

```
PASS blocksweep.arranged.matches_opd_render
PASS blocksweep.clip_fades.matches_opd_render
PASS blocksweep.clip_trim.matches_opd_render
PASS blocksweep.demo.matches_opd_render
PASS blocksweep.imported.matches_opd_render
PASS blocksweep.mixer.matches_opd_render
PASS blocksweep.panorama.matches_opd_render
PASS blocksweep.pitched.matches_opd_render
PASS blocksweep.resampled.matches_opd_render
PASS blocksweep.sampler_choke.matches_opd_render
PASS blocksweep.sampler_repitch.matches_opd_render
PASS blocksweep.sampler_trim.matches_opd_render
PASS blocksweep.synth_v1.matches_opd_render
PASS blocksweep.tempo.matches_opd_render
PASS blocksweep.time_sig_3_4.matches_opd_render
PASS blocksweep.time_sig_6_8.matches_opd_render
PASS blocksweep.every_golden_project_swept
PASS blocksweep.every_project_is_byte_identical
PASS blocksweep.callback_counts_match_the_frame_totals
PASS blocksweep.references_are_canonical_pcm16_stereo_wavs
PASS blocksweep.transport_counters_match_the_played_frames
PASS blocksweep.block_sizes_stay_in_range_and_actually_vary
PASS blocksweep.two_fixed_seed_sweeps_are_bit_identical
PASS blocksweep.negative_control_bit_flip_detected
PASS blocksweep.negative_control_block_swap_detected
```

Controls bite — two throwaway mutants of this test (compiled in `/tmp` against the same build dir, never
committed, deleted afterwards):

```
# mutant A: the old single-size habit, every callback exactly 128 frames
FAIL blocksweep.block_sizes_stay_in_range_and_actually_vary  (/tmp/mutA.cpp:300)
1 FAILURE(S)
# mutant B: comparison helper degraded to "same size" (memcmp bypassed)
FAIL blocksweep.negative_control_bit_flip_detected  (/tmp/mutB.cpp:334)
FAIL blocksweep.negative_control_block_swap_detected  (/tmp/mutB.cpp:335)
2 FAILURE(S)
```

**[x] Two consecutive runs print byte-identical `SWEEP` lines (fixed seed).**

```
$ "$B/op_player_blocksweep_tests" > /tmp/run1.txt 2>/tmp/run1.err; c1=$?      # exit 0
$ "$B/op_player_blocksweep_tests" > /tmp/run2.txt 2>/tmp/run2.err; c2=$?      # exit 0
$ cmp /tmp/run1.txt /tmp/run2.txt && echo "STDOUT BYTE-IDENTICAL across runs"
STDOUT BYTE-IDENTICAL across runs
```

Also cross-configuration: the Debug+ASAN build's whole stdout is byte-identical to the Release build's
(`cmp /tmp/san_sweep.txt /tmp/run1.txt` → identical), i.e. the sweep does not depend on optimisation level.

**[x] Standard gates.**

Build (dz-build skill; `-Wall -Wextra -Werror` — `option(DZ_WERROR … ON)` — `-ffp-contract=off`, `-j4`,
build dir outside the checkout):

```
$ CCACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.ccache bash agent-build-kit/loop/tools/dz_build.sh \
      openpattern --out builds/wp0241                      # all targets
EXIT=0 ; 100% tests passed, 0 tests failed out of 19 ; Total Test time (real) = 0.88 sec
$ grep -ci warning builds/wp0241/dz_build.log
0
```

Full `ctest` (18 pre-existing tests + the new `player_blocksweep`): `100% tests passed, 0 tests failed out of
19`, including `19/19 Test #19: player_blocksweep .... Passed 0.15 sec`. `./op_tests` standalone:
`ALL PASS (0 failures)`, exit 0.

```
$ bash harness/run_goldens.sh "$B/opd"
… 16 × PASS <name>: deterministic + golden match …
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures        goldens_exit=0

$ bash harness/test_cli_errors.sh "$B/opd" | tail -2
PASS samples_o_file_matches_stdout (stdout empty)
test_cli_errors: 0 failures                 cli_errors_exit=0

$ mkdir -p build && ln -sf "$B/opd" build/opd          # test_glue_scripts.sh wants $ROOT/build/opd
$ UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh | tail -2
PASS rel_no_repo_strays
test_glue_scripts: 0 failures                          glue_exit=0
$ rm build/opd                                          # symlink removed afterwards

# ASAN+UBSAN (same as the CI sanitizers job: Debug + DZ_SANITIZE, targets op_tests + the new test)
$ rm -rf builds/wp0241-san && DZ_BUILD_TYPE=Debug CCACHE_DIR=… bash agent-build-kit/loop/tools/dz_build.sh \
      openpattern --out builds/wp0241-san --sanitize --target op_tests --target op_player_blocksweep_tests
BUILD_EXIT=0 ; warnings in log: 0
$ ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
$ builds/wp0241-san/op_tests; echo $?
ALL PASS (0 failures)
0
$ builds/wp0241-san/op_player_blocksweep_tests | tail -1; echo $?      # 16 SWEEP + 25 PASS
ALL PASS (0 failures)
0                                          # stderr of both runs: 0 bytes → no sanitizer report
```

The scratch dirs are gone after every run (`ls -d /tmp/op_player_blocksweep_*` → `none`), and the test leaves
the repo alone (`PASS rel_no_repo_strays` in the glue gate; `git status --short` shows only the intended paths).

**[x] No behaviour change.** Every pre-existing gate passed exactly as before: ctest 19/19 (the 18 older suites
unchanged), goldens bit-exact against the committed fixtures, CLI exit-code contracts, glue e2e. The sidecar was
untouched but re-checked anyway: `uv run --offline ruff check .` → `All checks passed!`,
`uv run --offline pytest -q` → `146 passed, 2 skipped` (`UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`).

### Metrics

- 16/16 golden projects byte-identical live-vs-render; 4 200 000 frames swept through 15 424 `process()` calls;
  callback sizes drawn 32…512 samples (global min 32, global max 512).
- Test runtime 0.15 s (Release) / ~1 s (Debug+ASAN), i.e. negligible CI cost; 17 `opd render` subprocesses.
- New CHECK count 25; failure exit code equals the number of failures.

### Notes / follow-ups

- Compiler coverage: this rig has g++ 13.3 only (AGENTS.md: "no clang locally"), so the builds above are gcc;
  the clang leg stays a CI job. The new file is plain C++20 with no GCC-isms and compiles with
  `-Wall -Wextra -Werror` at 0 warnings in both Release (-O3) and Debug.
- No mismatch anywhere on this corpus, so no `PROPOSE:` fix note is needed for the transport itself: the loop
  region, metronome and per-block plumbing stay bit-identical to offline render under 32…512-sample callbacks.
- Environment notes for whoever runs these gates next (not repo changes):
  - `dz_build.sh` needs `CCACHE_DIR` pointed at a writable dir in this sandbox — `~/.cache/ccache` is read-only
    and its `tmp/` breaks configure with "failed to create temporary file".
  - `--sanitize` at the default `CMAKE_BUILD_TYPE=Release` fails in `src/engine/render.cpp:786`
    (`out.arrangement = {clip};`) with a pre-existing GCC 13.3 `-Werror=array-bounds=` false positive from the
    inlined `std::copy` — nothing to do with this package, and the CI sanitizers job does not hit it because it
    configures `-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`; the run above follows CI.
    `PROPOSE:` if the harness ever wants Release+sanitizers locally, that engine line wants a small
    rewrite (explicit `reserve`/`push_back`) in a separate package — not touched here (out of scope).
- Deliberately **not** covered by this package (future transport work will extend it): loop region, metronome
  click, stop/restart and latency compensation through the same byte-identity lens — `dz::run_render_playback`
  and `opd render` are still only compared for plain one-shot playback.
