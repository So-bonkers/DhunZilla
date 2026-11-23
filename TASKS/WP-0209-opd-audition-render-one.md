---
id: WP-0209
title: `opd audition` - render ONE note of a sample through the existing sampler voice (resample/trim/repitch), no project needed, bit-exact with the equivalent one-track project render (new engine module)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, cli:main, new:src/engine/audition.hpp, new:src/engine/audition.cpp, new:tests/test_audition.cpp]
---

## Context - why this exists

Phase 1's channel rack and sample browser need "what does this sample sound like when I hit C3?" without
creating a project file: the user points at a WAV, optionally names a note and the sample's root pitch and a
slice, and gets one short WAV back. This package adds a NEW engine module `op::audition` that **builds** the
canonical one-track sampler *project* for such a request (pure model construction, no audio code) plus an
`opd audition` command that renders it and writes pcm16. It adds **no new DSP**: every output sample is
produced by the existing sampler voice in `render.cpp`, i.e. the same resample (WP-0138) -> trim (WP-0156) ->
repitch (WP-0167) path a project render uses, so the result is bit-exact with the equivalent one-track
project. Read-only (the input asset is never modified), deterministic and reuses engine code only.

Existing code this package reuses (read it first, do not reimplement or modify it): `op::render_project` and
`op::total_samples_for` in `src/engine/render.hpp` (which owns the sampler voice, its repitch ratio and the
trim slice arithmetic); `op::Project` / `op::Track` / `op::Pattern` / `op::Note` / `op::VoiceKind` in
`src/engine/model.hpp`; `op::wav::write_wav_stereo`, `op::wav::SampleFormat` in `src/engine/wav.hpp`; the
existing static helpers `parse_bar_value` (whole-string finite double), `parse_decimal` (all-digits integer
in `[0, max]`) and `fail(kind, msg)` already defined in `src/cli/main.cpp`.

## Spec - normative (this task file is the contract)

### Files (exactly these 5; stage no others)
- `src/engine/audition.hpp`, `src/engine/audition.cpp` (new) - namespace `op::audition`, the interface below.
- `tests/test_audition.cpp` (new) - own `main`, own `CHECK(cond, name)` printing `PASS <name>` / `FAIL <name>`,
  returns the failure count; copy the style of `tests/test_timeline.cpp`. Unique temp dir:
  `std::filesystem::temp_directory_path() / ("op_audition_tests_" + pid)`, removed at the end - never a fixed
  `/tmp/op_*` path. It runs the CLI through the `OPD_BIN` macro. Do NOT edit `tests/test_main.cpp`.
- `CMakeLists.txt` - purely ADDITIVE (change/delete no existing line):
  1. directly after the line `target_sources(opengine PRIVATE src/engine/meters.cpp)  # WP-0194` add
     `target_sources(opengine PRIVATE src/engine/audition.cpp)  # WP-0209`;
  2. immediately BEFORE the comment line starting `# Our targets only` add (with one blank line before and
     after your block):
     ```
     # WP-0209: opd audition — project-free one-note sampler preview. Runs the opd binary, so it needs OPD_BIN.
     add_executable(op_audition_tests tests/test_audition.cpp)
     target_link_libraries(op_audition_tests PRIVATE opengine)
     target_compile_definitions(op_audition_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
     add_dependencies(op_audition_tests opd)
     add_test(NAME audition COMMAND op_audition_tests)
     ```
  3. directly after the line `list(APPEND DZ_OUR_TARGETS op_player_click_tests)  # WP-0201` add
     `list(APPEND DZ_OUR_TARGETS op_audition_tests)  # WP-0209`.
- `src/cli/main.cpp` - additive only, no existing line modified or removed:
  1. `#include "engine/audition.hpp"` directly after the `#include "engine/analyze.hpp"` line;
  2. these two usage lines inserted immediately AFTER the `opd timeline` usage line and BEFORE the
     `opd samples` usage line, exactly:
     ```
        "  opd audition <in.wav> -o <out.wav> [--note N --root R] [--start-s A --end-s B] [--seconds S]\n"
        "             (one note through the sampler voice - resample/trim/repitch - no project needed)\n"
     ```
  3. one `static int cmd_audition(int argc, char** argv)` function placed immediately BEFORE
     `int main(int argc, char** argv) {`;
  4. one dispatch line `if (cmd == "audition") return cmd_audition(argc - 1, argv + 1);` immediately AFTER
     the `if (cmd == "timeline")` line.
- Do NOT edit any other file: no other engine module, no `docs/`, no `harness/`, no fixtures, no CI.

### Engine API (exactly this, in `src/engine/audition.hpp`)
```cpp
#pragma once
#include <string>

#include "engine/model.hpp"
#include "engine/wav.hpp"

namespace op::audition {

constexpr int kSampleRate = 48000;
constexpr double kBpm = 87.890625;                 // == 5625/64, exactly representable
constexpr int kBeatsPerBar = 4;
constexpr double kMaxSeconds = 300.0;               // largest accepted --seconds value
constexpr long long kDefaultFrames = 96000;         // length when neither --seconds nor --end-s is given

struct Options {
    std::string path;                   // sampler asset path, stored in the voice verbatim
    int note = -1;                      // MIDI pitch of the single note; -1 = no pitch -> shift 0
    int root = -1;                      // asset's root MIDI pitch; -1 = absent -> no repitch
    double start_s = 0.0;               // trim slice start, seconds on the asset timeline
    double end_s = 0.0;                 // trim slice end; meaningful only when has_end_s
    bool has_end_s = false;             // false = play to the end of the asset
    long long frames = kDefaultFrames;  // output length in frames; build_project uses max(frames, 1)
};

long long to_frames(double seconds);
Project build_project(const Options& o);
bool render(const Options& o, const std::string& base_dir, wav::StereoBuffer& out, std::string& err);

}  // namespace op::audition
```

**Why `kBpm == 87.890625` (normative, do not "simplify" it to 120).** The renderer computes the buffer length
as `ceil(beats_at_bar(p, p.length_bars) * sample_rate * 60.0 / bpm)` and the note length as
`llround(dur_beats * spb)` with `spb = sample_rate * 60.0 / bpm`. With `kBpm == 5625/64` at 48 kHz, `spb` is
**exactly 32768.0**, so `dur_beats = frames / 32768.0` is exact (division by a power of two) and both
expressions land back on exactly `frames` samples - no off-by-one frame of silence, ever, for any request.
With bpm 120 the intermediate rounding makes the length sometimes `frames + 1`. Do not add any tempo map:
`p.tempo_map` stays empty (the constant-bpm path).

### `to_frames(seconds)` (exact)
`llround(seconds * kSampleRate)` with the product clamped to `[-4e18, 4e18]` before `llround` so absurd
magnitudes stay defined. Same rounding rule as the renderer's trim (spec project-model.md §Sampler trim).
Examples: `to_frames(2.0) == 96000`, `to_frames(0.0125) == 600`, `to_frames(0.0375) == 1800`,
`to_frames(0.0001) == 5` (4.8 rounds to 5), `to_frames(0.0) == 0`, `to_frames(-0.5) == -24000`.

### `build_project(o)` (exact, pure: no I/O, no clock, no randomness)
With `n = max(o.frames, 1)`, `spb = static_cast<double>(kSampleRate) * 60.0 / kBpm` and
`dur_beats = static_cast<double>(n) / spb`:
- `Project p;` then `format_version = 0`, `bpm = kBpm`, `sample_rate = kSampleRate`, `seed = 42`,
  `beats_per_bar = kBeatsPerBar`, `length_bars = dur_beats / static_cast<double>(kBeatsPerBar)`.
- One `Track`: `name = "audition"`, `voice.kind = VoiceKind::Sampler`, `voice.path = o.path` (verbatim - the
  renderer resolves it against `base_dir`), `voice.start_s = o.start_s`, `voice.end_s = o.end_s`,
  `voice.has_end_s = o.has_end_s`, `voice.root_note = o.root`. Everything else default: `gain_db = 0.0f`,
  `pan = 0.0f`, `mute = false`, `solo = false`, `choke_group = 0`.
- One `Pattern` with `track = 0` holding exactly one `Note`: `t_beats = 0.0`, `dur_beats = dur_beats`,
  `midi_note = o.note`, `velocity = 1.0`. No `length_beats`, no swing.
- `p.arrangement` and `p.tempo_map` stay EMPTY (empty arrangement = the legacy placement of pattern 0 once at
  bar 0, so there is no clip gain, fade or trim window to perturb the render); `master_gain_db = 0.0f`.
- Consequence that MUST hold: `op::total_samples_for(build_project(o)) == max(o.frames, 1)`.

### `render(o, base_dir, out, err)`
`build_project` then `op::render_project(p, base_dir, out, err)` - nothing else. Returns whatever
`render_project` returns (so an unreadable asset, an unsupported sample rate or `"sampler: trim slice is
empty"` propagate unchanged; do not reword them and do not pre-check the file yourself).

### CLI `cmd_audition` - syntax, order of checks, exact strings, exit codes
```
opd audition <in.wav> -o <out.wav> [--note N --root R] [--start-s A --end-s B] [--seconds S]
```
Argument scan (left to right): `-o V`, `--note V`, `--root V`, `--start-s V`, `--end-s V`, `--seconds V` each
consume the next argv; the first bare argument that is not an option is the input file. Any other token that
starts with `-`, and any second positional: `{ usage(); return fail("usage", "unknown arg '<token>'"); }`.
Repeating a flag: last value wins.

Then these checks, **in this exact order**, all before the input file is looked at; nothing is ever written on
error (no output file, no partial file). `fail(kind, msg)` prints `error: <kind>: <msg>` to stderr and yields
the exit code shown; where `usage()` is listed it is called first (it writes the usage block to stderr):
1. no input file at all -> `usage(); fail("usage", "audition needs <in.wav>")` -> **2**
2. `-o` missing/empty -> `usage(); fail("usage", "missing -o <out.wav>")` -> **2**
3. `--seconds V` not parseable by `parse_bar_value` -> `usage(); fail("usage", "bad --seconds value '<V>'")` -> **2**
4. `--seconds V` present and not `0.0 < V <= op::audition::kMaxSeconds` ->
   `usage(); fail("usage", "--seconds must be in (0, 300]")` -> **2**
5. `--start-s V` not parseable by `parse_bar_value` -> `usage(); fail("usage", "bad --start-s value '<V>'")` -> **2**
6. `--start-s V` present and `!(V >= 0.0)` -> `usage(); fail("usage", "--start-s must be >= 0")` -> **2**
7. `--end-s V` not parseable by `parse_bar_value` -> `usage(); fail("usage", "bad --end-s value '<V>'")` -> **2**
8. `--end-s V` present and `!(V > start_s)` (with `start_s = 0.0` when `--start-s` is absent) ->
   `usage(); fail("usage", "--end-s must be greater than --start-s")` -> **2**
9. `--note V` fails `parse_decimal(V, 127, n)` -> `usage(); fail("usage", "bad --note value '<V>'")` -> **2**
   (so `128`, `-1`, `60.5`, `C3`, `` are all rejected; `0` and `127` are accepted)
10. `--root V` fails `parse_decimal(V, 127, n)` -> `usage(); fail("usage", "bad --root value '<V>'")` -> **2**
11. exactly one of `--note` / `--root` present -> `usage(); fail("usage", "--note and --root must be given
    together")` -> **2** (they are a pair: with no root the engine never repitches, so half a pair is meaningless)
12. input file does not exist (`!fs::exists`) -> `fail("not_found", <in_path>)` -> **3**

Then build `op::audition::Options o`: `path = in_path`; `note`/`root` from the parsed values, `-1` when the
flag was absent; `start_s`/`end_s`/`has_end_s` (`true` iff `--end-s` was given); and the output length:
- `--seconds S` present -> `o.frames = op::audition::to_frames(S)` (explicit length wins over everything),
- else `--end-s B` present -> `o.frames = op::audition::to_frames(B) - op::audition::to_frames(start_s)`
  (the trimmed span, in frames - the same frame indices the renderer slices on),
- else -> `o.frames = op::audition::kDefaultFrames` (2 s).

Render with an EMPTY base_dir (`op::audition::render(o, "", buf, err)`) so a relative `in_path` resolves
against the caller's working directory and an absolute one passes through. On failure:
`return fail("render", err)` -> **5** (this covers a file that exists but is not decodable audio, an
unsupported rate and an empty trim slice; the message is the sampler's own - do not assert or reword it).
Write `op::wav::write_wav_stereo(out_path, buf, op::wav::SampleFormat::Pcm16, err)`; on failure
`return fail("write", err)` -> **5**. Output is ALWAYS stereo pcm16 at 48000 Hz (mono assets are duplicated
by the sampler voice itself). There is deliberately no `--format`, no `--seed`, no `--json`.

On success print exactly one line to stdout and exit **0**:
```
auditioned <frames> frames @<rate> Hz -> <out_path>\n
```
`<frames>` = `buf.frames()` (i.e. the requested length), `<rate>` = `buf.sample_rate` (`48000`), `<out_path>`
= the `-o` argument verbatim. Nothing else on stdout; nothing on stderr.

**Bit-exactness (the core requirement).** For any accepted arguments, the output file is byte-identical to
`opd render <equiv.opj> -o <same.wav>` where `<equiv.opj>` is the one-track project `build_project` describes
(same numbers: `"bpm": 87.890625`, `"sample_rate": 48000`, `"beats_per_bar": 4`, `"seed": 42`,
`"length_bars": dur_beats/4`, one sampler track with the same `path` / `start_s` / `end_s` / `root_note`, one
pattern with the single note `{t: 0.0, dur: dur_beats, note: N}`). Worked example - asset `/tmp/a/kick.wav`,
`opd audition /tmp/a/kick.wav --note 72 --root 60 --start-s 0.01 --end-s 0.5 -o out.wav` produces exactly the
bytes of a project whose `length_bars` is `0.179443359375`, whose track voice is
`{"kind": "sampler", "path": "/tmp/a/kick.wav", "start_s": 0.01, "end_s": 0.5, "root_note": 60}` and whose
only note is `{"t": 0.0, "dur": 0.7177734375, "note": 72}`: both are 23520 frames of silence-free audio.
The mixer path is the ordinary one, so a default track's channels are scaled by `equal_power_pan(0)` - about
-3.01 dB per channel. That is intended; do not compensate for it anywhere.

### Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, no new libm use in the engine module except `std::llround` (already
used that way by `render.cpp`); no fast-math; `-Werror` clean and `-DDZ_SANITIZE=ON` clean. The command is
read-only: it never modifies the input asset, never writes anything except the `-o` file, and two runs of the
same command produce byte-identical files.

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`).
- [x] `opd audition <asset> -o out.wav` writes a stereo pcm16 48000 Hz WAV of exactly 96000 frames and prints
      `auditioned 96000 frames @48000 Hz -> out.wav`; with `--seconds S` the frame count is exactly
      `llround(S*48000)`; with `--start-s A --end-s B` (no `--seconds`) it is exactly
      `llround(B*48000) - llround(A*48000)`; `--seconds` wins when both are given.
- [x] Bit-exact equivalence holds end-to-end: for the worked example above (and for at least one unpitched,
      untrimmed case and one 44.1 kHz asset), the `opd audition` output file is byte-identical to
      `opd render` of the equivalent `.opj`.
- [x] Repitch/trim/resample really go through the sampler voice: `--note R+12 --root R` on a 48 kHz asset
      yields source frame `2n` at output frame `n` (bit-exact, scaled only by the default channel gain);
      `--start-s 0.0125 --end-s 0.0375` plays exactly frames `[600, 1800)` of the project-rate buffer and then
      silence; a 44.1 kHz asset renders (rate conversion happens inside the engine).
- [x] Every usage error above exits 2 with the exact message and writes no output file; a missing input exits
      3; an existing-but-undecodable asset and an empty trim slice exit 5, both writing nothing.
- [x] `tests/test_audition.cpp` with at least 40 CHECKs covering: the constants (`kBpm == 5625.0/64.0`),
      `to_frames` (the six examples above incl. negative and half-up rounding), the exact shape of
      `build_project` (track name/kind/path, voice fields, one note at t 0 with the exact `dur_beats`, empty
      arrangement and tempo map, mixer defaults), `total_samples_for(build_project(o)) == frames` for at least
      seven frame counts including 1 and a 300 s request plus the zero/negative clamp to 1, bit-exact equality
      with a hand-written equivalent `op::Project` render for plain / repitch-up / repitch-down / resampled
      cases, the +12-every-second-frame and -12-interpolation sample checks, the trim slice + trailing silence,
      two renders identical, the CLI stdout line byte-exact, the pcm16/stereo/48000 header, all six length
      rules, CLI determinism (byte-identical files), read-only (asset unchanged and only the output file
      appears), the equivalent-project byte comparison via `opd render`, and every error case above.
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_audition_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_audition_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `docs/` (no spec file for this package), golden fixtures
  (`tests/fixtures`), `harness`, `.github`, `ai-sidecar`, the JUCE app sources under `src/app`, `src/doc`, `tools`,
  `tests/test_main.cpp`, and every other engine module — in particular `render.*`, `model.*`, `wav.*`,
  `resample.*`, `dsp.hpp` are read-only here.
- No new DSP: do not implement resampling, pitch shifting, slicing, envelope or gain math in this package -
  the sampler voice already does all of it. Do not call `std::pow` for pitch.
- No project file creation/reading, no `--format` / `--seed` / `--json` / `--normalize` options, no stem or
  multi-note audition (one note only), no MIDI input, no live playback, no looping/repeat of the preview, no
  writing into the asset's directory, no `opd info` changes.

## Git
- Branch `wp-0209-opd-audition-render-one` from the current HEAD (the integration base: do not switch to main first). Commit:
  `WP-0209: opd audition - one-note sample preview through the sampler voice, no project`. Stage explicit
  paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the 5 files, additive)

- **`src/engine/audition.hpp`** (new, 47 lines) — `op::audition` interface exactly as specified:
  constants (`kBpm == 87.890625`, `kSampleRate`, `kBeatsPerBar`, `kMaxSeconds`, `kDefaultFrames`),
  `Options`, `to_frames`, `build_project`, `render`.
- **`src/engine/audition.cpp`** (new, 68 lines) — pure model construction only; zero DSP. `to_frames`
  clamps the product to [-4e18, 4e18] before `llround`; `build_project` builds the canonical one-track
  sampler project (`spb = 48000*60/kBpm` is exactly 32768.0 so every size lands back on exactly `frames`);
  `render` = `build_project` + `op::render_project`, nothing else.
- **`tests/test_audition.cpp`** (new, 599 lines) — own `main`/`CHECK`, scratch dir
  `temp_directory_path()/("op_audition_tests_" + pid)` removed at the end, CLI via `OPD_BIN`. 72 CHECKs.
- **`CMakeLists.txt`** — purely additive: `target_sources(opengine ... audition.cpp) # WP-0209`, the
  `op_audition_tests` block (with `OPD_BIN` + `add_dependencies` + `add_test(NAME audition ...)`) before
  `# Our targets only`, and `list(APPEND DZ_OUR_TARGETS op_audition_tests)  # WP-0209`. Diff: 9 insertions, 0 deletions.
- **`src/cli/main.cpp`** — additive only (117 insertions, 0 deletions): include after `analyze.hpp`, the two
  usage lines between `timeline` and `samples`, `cmd_audition` immediately before `main`, dispatch line
  immediately after the `timeline` one.

### How verified (commands + real output)

Build (dz-build skill, `-j4`, shared lock; Release build log has **0 warning lines**, `-Werror` on our targets):

```
$ bash agent-build-kit/loop/tools/dz_build.sh "$PWD" --target opd --target op_tests --target op_audition_tests --out .../build-wp0209 | tail -1
BUILD_DIR=/home/shubhankar_dgx/qwen-scratch/build-wp0209
$ grep -c warning /home/shubhankar_dgx/qwen-scratch/build-wp0209/dz_build.log
0
```

New test binary — 72 CHECKs, exit code = failures = 0:

```
$ ./build-wp0209/op_audition_tests | tail -1 ; ./build-wp0209/op_audition_tests | grep -c '^PASS'
ALL PASS
72
```

Highlights of the run (all 72 lines PASS; representative):

```
PASS const: kBpm == 5625/64 exactly
PASS to_frames: 0.0001 -> 5 (4.8 rounds up)
PASS to_frames: absurd magnitudes clamp to +/-4e18 before llround
PASS shape: length_bars == 0.179443359375 (worked example)
PASS total: exact frame count for 7 sizes incl. 1 and 300 s
PASS total: frames 0 and negative clamp to 1
PASS exact.plain: audition == hand project render
PASS exact.repitch_up: audition == hand project render
PASS exact.repitch_down: audition == hand project render
PASS exact.resampled: audition == hand project render (44.1 kHz asset)
PASS voice.+12: output frame n == asset frame 2n * channel gain
PASS voice.-12: odd output frames are the exact midpoint interpolations
PASS trim: frames [600,1800) of the asset play first, scaled only by channel gain
PASS determinism: two engine renders are bit-identical
PASS cli.default: exit 0, exact stdout line, nothing on stderr
PASS cli.default: canonical pcm16 stereo 48000 Hz, 96000 frames
PASS cli.len: --seconds wins over the slice span
PASS cli.determinism: byte-identical outputs
PASS cli.readonly: asset unchanged and nothing written next to it
PASS equiv.cli: worked example audition == opd render of the equivalent .opj, byte-for-byte
PASS equiv.cli: unpitched untrimmed audition == equivalent .opj render
PASS equiv.cli: 44.1 kHz repitched audition == equivalent .opj render
PASS err.note_bad_C3 ... PASS err.empty_slice (all 24 error-contract checks)
```

Full ctest suite (Release; the new `audition` test is #17):

```
$ cd build-wp0209 && ctest | grep -E "audition|tests passed"
      Start 17: audition
17/17 Test #17: audition .........................   Passed    0.12 sec
100% tests passed, 0 tests failed out of 17
```

Golden renders bit-identical (Release opd and sanitized opd both run):

```
$ bash harness/run_goldens.sh build-wp0209/opd | tail -1
run_goldens: 16 fixtures, 0 failures
$ bash harness/run_goldens.sh build-san-wp0209/opd | tail -1
run_goldens: 16 fixtures, 0 failures
```

CLI exit-code contracts (Release + sanitized):

```
$ bash harness/test_cli_errors.sh build-wp0209/opd | tail -1
test_cli_errors: 0 failures
$ bash harness/test_cli_errors.sh build-san-wp0209/opd | tail -1
test_cli_errors: 0 failures
```

Glue-script e2e (`build/opd` symlinked to the new binary for the run, original restored afterwards):

```
$ bash harness/test_glue_scripts.sh | tail -1
test_glue_scripts: 0 failures
glue_exit=0
```

Sanitizers — Debug + `-DDZ_SANITIZE=ON` (exactly as `.github/workflows/ci.yml` does), both binaries, and the
CLI subprocesses (`OPD_BIN`) run under it too:

```
$ DZ_BUILD_TYPE=Debug dz_build.sh "$PWD" --sanitize --target op_tests --target op_audition_tests --out .../build-san-wp0209
SAN_DIR=/home/shubhankar_dgx/qwen-scratch/build-san-wp0209   (0 warning lines in the log)
$ ./build-san-wp0209/op_tests > log; echo $?; tail -1 log
0
ALL PASS (0 failures)
$ ./build-san-wp0209/op_audition_tests > log; echo $?; tail -1 log
0
ALL PASS
```

Worked-example cross-check against `opd render` outside the test suite (independent confirmation):

```
$ opd audition "$D/kits/kick.wav" -o "$D/a.wav" --note 72 --root 60 --start-s 0.01 --end-s 0.5
auditioned 23520 frames @48000 Hz -> /tmp/tmp.2DWC8Q6QQj/a.wav
$ opd render "$D/eq.opj" -o "$D/r.wav"        # eq.opj: bpm 87.890625, length_bars 0.179443359375, note dur 0.7177734375
rendered 23520 frames @48000 Hz -> /tmp/tmp.2DWC8Q6QQj/r.wav (seed=42)
$ cmp "$D/a.wav" "$D/r.wav" && echo IDENTICAL
IDENTICAL
```

### Metrics

- Files: 2 new engine (47 + 68 lines), 1 new test (599 lines, 72 CHECKs), CMakeLists +9/-0, main.cpp +117/-0.
- ctest: 17/17 pass; goldens 16/16 bit-exact; CLI contracts 0 failures; glue e2e 0 failures (Release and sanitized).
- No new libm in the engine module beyond `std::llround`; no changes to `render.*`, `model.*`, `wav.*`, `resample.*`, `dsp.hpp`.

### Notes / follow-ups

- Pre-existing, not caused by this package: a **Release** (`-O3`) + `-DDZ_SANITIZE=ON` build of the untouched
  `src/engine/render.cpp` trips a known GCC 13.3 `-Werror=array-bounds` false positive in
  `pattern_mode_project` (WP-0192). CI's sanitizers job configures **Debug** (`ci.yml` line 79) and passes;
  the sanitizer evidence above uses that exact configuration. No action taken (out of scope).
- The `-o ""` empty-value case is treated as "missing -o" per check 2 ("missing/empty").
- `wpdraft6/` was already untracked in the checkout; left untouched and unstaged.
