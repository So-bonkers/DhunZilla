---
id: WP-0243
title: `opd meters --per-bar` - per-bar peak / RMS dBFS arrays for every track and the master, mapped with the renderer's own bar→sample mapping
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, cli:main, src/engine/meters.hpp, src/engine/meters.cpp, new:tests/test_meters_bar.cpp]
---

## Context - why this exists
The mixer's meter bridges and the copilot need a *timeline* of levels, not one number per track: "how loud
was bar 5 of this track?" The existing `opd meters` (WP-0194) gives one whole-project peak / RMS / LUFS per
track and for the master. This package adds the `--per-bar` flag to that command: for every track and for
the master it appends an array of per-bar peak dBFS and RMS dBFS, using the RENDERER's own bar→sample
mapping so tempo maps and time signatures are honoured. Without the flag the output stays **byte-identical**
to what it is today. Existing code this package reuses (read it first, do not reimplement it):
`op::meters` (`src/engine/meters.{hpp,cpp}`), `op::analyze::analyze_samples` (`src/engine/analyze.hpp`),
`op::render_stems` and `op::sample_at_bar` (`src/engine/render.hpp`), `wav::StereoBuffer`
(`src/engine/wav.hpp`). No new DSP, no new engine module, no file output, no project mutation.

## Spec - normative (this task file is the contract)

### Files (exactly these 5; stage no others)
- `src/engine/meters.hpp` - additive: the new `BarMeter` struct, the new `Report` / `TrackMeter` fields and
  the two new functions in the "API" block below. Do not change or remove any existing declaration.
- `src/engine/meters.cpp` - the per-bar implementation plus the two new `to_json` sections in the "JSON"
  block below. Do not change the whole-buffer measurement path (`meter_buffer`, `measure_project`) or the
  `%.4f` / negative-zero formatting helpers.
- `src/cli/main.cpp` - only inside the existing `cmd_meters` block: option parsing for `--per-bar`, and the
  exact usage line below. No new command, no new dispatch line (`meters` is already dispatched), no other
  command touched.
- `tests/test_meters_bar.cpp` (new) - own `main`, own `CHECK(cond, name)` macro printing `PASS <name>` /
  `FAIL <name>` (copy the style of `tests/test_meters.cpp`), returns the failure count. Use a unique temp
  dir (`std::filesystem::temp_directory_path()` + a name containing the pid), never fixed `/tmp/op_*`
  paths; remove it at the end. Write the test project as a JSON text file and `op::load_project` it.
  Do NOT edit `tests/test_meters.cpp` or `tests/test_main.cpp`.
- `CMakeLists.txt` - purely ADDITIVE (change or delete nothing):
  1. immediately AFTER the line `add_test(NAME tracks COMMAND op_tracks_tests)` add, with one blank line
     before them:
     ```cmake
     # WP-0243: opd meters --per-bar — per-bar peak/RMS arrays per track and the master.
     add_executable(op_meters_bar_tests tests/test_meters_bar.cpp)
     target_link_libraries(op_meters_bar_tests PRIVATE opengine)
     add_test(NAME meters_bar COMMAND op_meters_bar_tests)
     ```
  2. immediately AFTER the line `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227` add
     `list(APPEND DZ_OUR_TARGETS op_meters_bar_tests)  # WP-0243`.
- Do NOT edit `src/engine/analyze.*`, `render.*`, `wav.*`, `loudness.*`, `dsp.hpp`, `model.*` or any other
  engine file. Constraints on `meters.cpp` (graded by grep): it must contain the text `analyze_samples(`
  and the text `sample_at_bar(`; it must NOT contain any direct libm call - none of
  `std::sin( std::cos( std::log10( std::log( std::pow( std::sqrt( std::exp( std::tan( std::fabs( std::abs( std::floor( std::round(`
  (`snprintf`, `std::ceil` and `+ - * /` are fine - the dB math stays inside `analyze.cpp`).

### API (exactly this, added to namespace `op::meters` in `src/engine/meters.hpp`)
```cpp
struct BarMeter {                     // one bar of level history (WP-0243)
    long long bar = 0;                // 0-based bar index
    long long start_sample = 0;       // op::sample_at_bar(p, bar)
    long long end_sample = 0;         // op::sample_at_bar(p, bar + 1.0)
    std::optional<double> peak_dbfs;  // empty = the bar is silent (or empty)
    std::optional<double> rms_dbfs;   // empty = the bar is silent (or empty)
};
```
Add `std::vector<BarMeter> bars;` to `TrackMeter` and to `Report` add, in this order after `duration_s`,
`bool per_bar = false;` and `long long bar_count = 0;`, plus after `master`
`std::vector<BarMeter> master_bars;`. Then add exactly these two functions (existing declarations
unchanged):
```cpp
BarMeter meter_bar(const wav::StereoBuffer& buf, long long bar, long long start_sample, long long end_sample);
bool measure_project_per_bar(const Project& p, const std::string& base_dir, Report& out, std::string& err);
```
- `meter_bar` measures frames `[start_sample, end_sample)` of `buf` (stereo, interleaved) with
  `op::analyze::analyze_samples` over **exactly those frames** and copies `bar`, `start_sample`,
  `end_sample` into the result. The result's `peak_dbfs` / `rms_dbfs` are `analyze`'s `peak_dbfs` /
  `rms_dbfs` copied unchanged, so an all-silent or empty (`start_sample == end_sample`) range leaves both
  empty. Precondition (the callers pass mapped values): `0 <= start_sample <= end_sample <= buf.frames()`.
- `measure_project_per_bar` renders with `op::render_stems` exactly like `measure_project` (same errors,
  same `err` text, `out` reset to a fresh `Report` on success), sets `out.per_bar = true`,
  `out.bar_count = std::ceil(p.length_bars)` (the smallest integer >= `length_bars`, i.e. exactly the bar
  count `opd timeline` prints; a final PARTIAL bar is included and measured up to the end of the render),
  and fills `bars` for every track (project track order) and `master_bars` for the master with one
  `BarMeter` per bar `0 .. bar_count-1`. `sample_rate`, `frames`, `duration_s` and the whole-buffer
  `meter` / `master` values are filled exactly as `measure_project` fills them.
- The bar window must come from the renderer's mapping and nowhere else: `start_sample =
  op::sample_at_bar(p, static_cast<double>(bar))` and `end_sample = op::sample_at_bar(p, static_cast<double>(bar + 1))`.
  That function is already clamped to `total_samples_for(p)`, which equals `Report::frames`, so the last bar
  ends exactly at the last rendered frame. Do NOT compute bar lengths from `p.bpm`, from
  `frames / bar_count`, or from any other formula - the tempo map and the time signature must fall out of
  `op::sample_at_bar` on their own.

### CLI (exact syntax, strings and exit codes)
Usage line in `src/cli/main.cpp`, replacing the current `opd meters` line verbatim:
```
  opd meters <project.opj> [--per-bar]   (per-track and master peak/RMS dBFS + integrated LUFS as JSON)
```
- `--per-bar` is the only option; it may come before or after the project path (both orders produce byte
  -identical output). The project path is still exactly one positional.
- Errors print nothing on stdout (the usual `usage()` block to stderr is fine) and exit 2 with exactly these
  detail messages: no project path -> `meters needs exactly <project.opj>`; `--per-bar` given twice ->
  `--per-bar given twice`; any other token starting with `-` -> `unknown arg '<the token>'`; a second
  positional -> `unknown arg '<the token>'`. A missing file still exits 3 and an unloadable project still
  exits 4, with the flag on or off; a failed render still exits 5.

### JSON (byte-exact; without `--per-bar` nothing changes at all)
With the flag, two things are added and nothing else moves:
1. a top-level `"bar_count": <n>` line inserted immediately after the `"duration_s"` line;
2. inside every track object and inside `"master"`, a `"bars": [...]` array appended after
   `"integrated_lufs"`, one object per bar, all on the same line, bar objects separated by `", "`.
A bar object is exactly `{"bar": <i>, "start_sample": <s>, "end_sample": <e>, "peak_dbfs": <n or null>,
"rms_dbfs": <n or null>}` - the same `%.4f` number format as the existing meters, with the existing rule
that a value printed as `-0.0000` is printed as `0.0000`. An empty array prints as `"bars": []`.
No `"integrated_lufs"` key inside a bar object (per-bar LUFS is out of scope), no `"duration_s"` per bar.
Byte-exact example (a hand-built report with one track named `a"b`, 48000 Hz, 192000 frames, 2 bars):
```
{
  "sample_rate": 48000,
  "frames": 192000,
  "duration_s": 4.000000,
  "bar_count": 2,
  "tracks": [
    {"index": 0, "name": "a\"b", "peak_dbfs": -6.0206, "rms_dbfs": -12.0412, "integrated_lufs": -15.0550, "bars": [{"bar": 0, "start_sample": 0, "end_sample": 96000, "peak_dbfs": -6.0206, "rms_dbfs": 0.0000}, {"bar": 1, "start_sample": 96000, "end_sample": 192000, "peak_dbfs": null, "rms_dbfs": null}]}
  ],
  "master": {"peak_dbfs": -6.0206, "rms_dbfs": -12.0412, "integrated_lufs": -15.0550, "bars": [{"bar": 0, "start_sample": 0, "end_sample": 96000, "peak_dbfs": -6.0206, "rms_dbfs": 0.0000}, {"bar": 1, "start_sample": 96000, "end_sample": 192000, "peak_dbfs": null, "rms_dbfs": null}]}
}
```
Without `--per-bar` the text must contain no `"bars"` and no `bar_count` anywhere, and every other byte must
be what `opd meters` prints today.

### Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, libm only via `dsp.hpp` (see the grep constraints above; `std::ceil`
is allowed), no fast-math; must pass `-Werror` and `-DDZ_SANITIZE=ON` (ASAN+UBSAN). Read-only: nothing is
written to disk, the project file and its directory are left byte-identical, and two runs of the same
command give identical bytes.

## Acceptance criteria
- [ ] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`).
- [ ] `opd meters <project.opj>` output is unchanged (no `"bars"` key, no `bar_count` key) and every
      per-bar value equals what `opd analyze` gives for the same frames: taking
      `opd render <project.opj> --format f32 -o mix.wav --stems-dir stems` and the windows printed by
      `opd timeline`, each bar's `peak_dbfs` / `rms_dbfs` match the values computed over frames
      `[start_sample, end_sample)` of that track's stem (and of `mix.wav` for the master) within 0.001 dB;
      a silent bar prints `null` for both.
- [ ] Bar windows use the renderer's mapping: for `tests/fixtures/goldens/tempo.opj` (8 bars, 120 bpm with
      72 bpm from bar 4, 4/4, 48 kHz, 1024000 frames) `bar_count` is 8 and the starts are
      0, 96000, 192000, 288000, 384000, 544000, 704000, 864000 with the last bar ending at 1024000; for
      `tests/fixtures/goldens/time_sig_3_4.opj` (3/4) `bar_count` is 3 with starts 0, 72000, 144000 and the
      last bar ending at 216000; for a project with `length_bars` 2.5 `bar_count` is 3 and the third bar
      covers a half bar ending exactly at `frames`.
- [ ] `--per-bar` adds only `"bar_count"` and `"bars"`: the per-bar output minus those keys is equal to the
      no-flag output; `--per-bar` before or after the path gives identical bytes.
- [ ] `tests/test_meters_bar.cpp` with at least 14 CHECKs covering: `meter_bar` field copy, its peak/RMS
      against `analyze_samples` of the same frames, silent bar -> two nulls, empty range -> two nulls,
      `bar_count == ceil(length_bars)`, the window equals `op::sample_at_bar` for every bar, windows tile
      the render with no gaps, inaudible track -> all bars null, a track that sounds in one bar only
      (null/null for the other bars), track bars equal `analyze` of that stem's window, master bars measured
      on the mix, `to_json` byte-exact for a hand-built per-bar `Report` (incl. `-0.0000` -> `0.0000`,
      nulls, escaped name), no bar keys when `per_bar` is false, determinism (two measurements give
      identical JSON).
- [ ] `opd meters` exit codes / stdout exactly as specified, including the three usage error strings, and
      the project file and its directory are left unchanged.
- [ ] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_meters_bar_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer
      build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_meters_bar_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md`, golden fixtures (`tests/fixtures/`), `harness/`, `.github/`, `docs/`, `ai-sidecar/`,
  the JUCE app sources, `tests/test_main.cpp`, `tests/test_meters.cpp`.
- No new engine module or CLI source file; no changes to `analyze.*`, `render.*`, `wav.*`, `loudness.*`,
  `dsp.hpp`, `model.*`, `CMakeLists.txt` beyond the additive lines above.
- No per-bar LUFS, no per-bar key other than `bars`/`bar_count`, no `--from-bar`/`--to-bar`/`--bars` option
  on `meters`, no CSV or text output mode, no GUI/app work, no new `opd` command.

## Git
- Branch `wp-0243-opd-meters-bar-level` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0243: opd meters --per-bar - per-bar peak/RMS arrays per track and master`. Stage explicit paths only (never `git add -A`).
  Include `TASKS/WP-0243-opd-meters-bar-level.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the 5 files + this task file)
- `src/engine/meters.hpp` — additive only: `BarMeter` struct (verbatim from the API block),
  `std::vector<BarMeter> bars;` in `TrackMeter`, `bool per_bar = false;` + `long long bar_count = 0;`
  after `duration_s` and `std::vector<BarMeter> master_bars;` after `master` in `Report`, and the two
  new declarations `meter_bar` / `measure_project_per_bar`. No existing declaration touched.
- `src/engine/meters.cpp` — `meter_bar` (copies bar/start/end, measures `[start,end)` with
  `op::analyze::analyze_samples` over exactly those frames, defensive clamp, empty range → two nulls),
  `measure_project_per_bar` (same `op::render_stems` call/errors as `measure_project`, same header +
  whole-buffer fill, `bar_count = std::ceil(p.length_bars)`, per-track and master bar windows from
  `op::sample_at_bar` only) and the two `to_json` sections (`"bar_count"` line after `"duration_s"`,
  `"bars": [...]` appended after `"integrated_lufs"` in every track and in `"master"`, both only when
  `per_bar`). `meter_buffer`, `measure_project`, `fmt4` / negative-zero / escaping helpers untouched.
- `src/cli/main.cpp` — inside `cmd_meters` only: `--per-bar` parsing (before or after the path;
  duplicates → `--per-bar given twice`; other `-` tokens or a second positional → `unknown arg '<tok>'`;
  no path → `meters needs exactly <project.opj>`), dispatch to `measure_project_per_bar`, and the exact
  new usage line. No new command, no new dispatch line, no other command touched.
- `tests/test_meters_bar.cpp` (new) — 18 CHECKs, own main + `CHECK` macro, unique temp dir with pid,
  project written as JSON text + `op::load_project`. Covers every listed area (see output below).
- `CMakeLists.txt` — only the two specified additive blocks (blank line before the test block;
  `list(APPEND DZ_OUR_TARGETS op_meters_bar_tests)  # WP-0243` right after the WP-0227 line).
- Constraints grep (graded): `meters.cpp` contains `analyze_samples(` (x2) and `sample_at_bar(` (x2);
  none of the banned `std::sin( … std::round(` texts occurs (`std::ceil` is the only math call,
  allowed): verified with `grep -Fn` for all 12 banned tokens → no matches.

### Acceptance criteria - checked off with real output
- [x] Everything implemented; existing goldens bit-identical:
```
$ bash harness/run_goldens.sh            # harness/run_goldens.sh (build/opd = wp0243 build)
PASS tempo: deterministic + golden match
PASS time_sig_3_4: deterministic + golden match
... (16 fixtures)
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures
```
- [x] No-flag output unchanged and per-bar values == `opd analyze` over the same frames.
  Byte-identity vs the PRE-WP binary (baseline `opd` built from base HEAD `d7e3fa3`):
```
$ for opj in tests/fixtures/goldens/*.opj partial.opj; do
    a=$(wp0243-base-build/opd meters $opj | md5sum); b=$(wp0243/opd meters $opj | md5sum); ...
PASS unchanged: arranged … PASS unchanged: tempo … PASS unchanged: partial   (17/17 PASS)
```
  Cross-check vs `opd analyze`: `opd render partial.opj --format f32 -o mix.wav --stems-dir stems`,
  each bar window `[start,end)` sliced and analyzed:
```
PASS every per-bar peak/RMS == opd analyze over frames [start,end) of stem/mix (<=0.001 dB)
```
  (a silent bar prints `null` for both — see the blip/muted CHECKs and the tempo example below).
- [x] Windows use the renderer's mapping:
```
PASS tempo.opj bar_count=8 windows 0,96000,192000,288000,384000,544000,704000,864000..1024000
PASS time_sig_3_4.opj bar_count=3 windows 0,72000,144000..216000
PASS length_bars 2.5 -> bar_count 3, third bar [192000,240000)==frames, timeline agrees
```
  Real tempo.opj master line (72-bpm bars 4-7 span 160000 frames each — tempo map falls out of
  `sample_at_bar`):
```
  "master": {"peak_dbfs": -26.1114, "rms_dbfs": -49.5274, "integrated_lufs": -44.5702, "bars": [{"bar": 0, "start_sample": 0, "end_sample": 96000, "peak_dbfs": -26.1114, "rms_dbfs": -48.2780}, … {"bar": 4, "start_sample": 384000, "end_sample": 544000, …}, {"bar": 7, "start_sample": 864000, "end_sample": 1024000, …}]}
```
- [x] `--per-bar` adds only `bar_count`/`bars`; flag order irrelevant:
```
PASS tempo plain output has no "bars" key
PASS tempo plain output has no bar_count key
PASS tempo --per-bar before/after path identical bytes
PASS per-bar output minus bar_count/bars == plain output (bytes)      (same 4 PASSes for partial.opj)
```
- [x] `tests/test_meters_bar.cpp` — 18 CHECKs, all required areas, all green:
```
$ ./op_meters_bar_tests
PASS meters_bar.test_project_json_loads
PASS meters_bar.measure_project_per_bar_succeeds
PASS meters_bar.bar_count_equals_ceil_length_bars
PASS meters_bar.header_and_whole_buffer_values_match_measure_project
PASS meters_bar.one_bar_per_bar_index_for_tracks_and_master
PASS meters_bar.meter_bar_copies_bar_and_window_fields
PASS meters_bar.meter_bar_equals_analyze_samples_of_window
PASS meters_bar.meter_bar_silent_window_two_nulls
PASS meters_bar.meter_bar_empty_range_two_nulls
PASS meters_bar.windows_equal_op_sample_at_bar_for_every_bar
PASS meters_bar.windows_tile_the_render_with_no_gaps
PASS meters_bar.muted_track_every_bar_two_nulls
PASS meters_bar.blip_track_audible_in_bar_one_only
PASS meters_bar.track_and_master_bars_equal_analyze_of_their_windows
PASS meters_bar.to_json_per_bar_matches_spec_example_byte_exact
PASS meters_bar.to_json_has_no_bar_keys_when_per_bar_false
PASS meters_bar.empty_bars_array_prints_as_bars_empty
PASS meters_bar.two_measurements_produce_identical_json
ALL PASS (0 failures)   exit 0
```
  (The byte-exact CHECK is the spec's `a\"b` example verbatim incl. `-0.0 → 0.0000`, nulls, `"bars": []`.)
- [x] Exit codes / stdout / read-only contracts:
```
PASS exit no_path(2)  PASS exit per_bar_twice(2)  PASS exit unknown_flag(2)  PASS exit second_positional(2)
PASS exit missing_file(3, flag on)  PASS exit unloadable(4, flag on)  PASS exit render_fail(5, flag on)
PASS stdout empty: <every one of the seven cases>
PASS detail string: --per-bar given twice
PASS detail string: meters needs exactly <project.opj>
PASS detail string: unknown arg '--frobnicate'
PASS detail string: unknown arg 'x'
PASS project_dir_unchanged                       # sha256 of every file in the project dir before == after
PASS two --per-bar runs identical bytes          # determinism through the CLI
```
  Usage line byte-checked against this task file: `opd` usage `grep 'opd meters' | md5sum` ==
  `sed -n '96p' TASKS/WP-0243-opd-meters-bar-level.md | md5sum` = `87c53a68e8a458f997572b526a57a409`.
- [x] Standard gates:
  - Build (dz_build.sh, unique dir, `-j4`, shared lock, ccache, `-Wall -Wextra -Werror`,
    `CMAKE_BUILD_TYPE=Release`): exit 0, no warning lines in `dz_build.log`.
    (First run failed only on ccache writing to `~/.cache` — sandbox; reran with
    `CCACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/ccache`, same as prior WPs.)
  - ctest: `19/19 tests passed` incl. `Test  #9: meters … Passed` and `Test #19: meters_bar … Passed`.
  - `harness/run_goldens.sh`: 16 fixtures, 0 failures (above).
  - `harness/test_cli_errors.sh`: `test_cli_errors: 0 failures`.
  - `harness/test_glue_scripts.sh`: `test_glue_scripts: 0 failures` (with
    `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`).
  - Sanitizer: `-DDZ_SANITIZE=ON` build of `op_tests` + `op_meters_bar_tests` at
    `CMAKE_BUILD_TYPE=Debug` (exactly how the CI sanitizers job configures it): build clean,
    `./op_tests ALL PASS (0 failures) exit 0`, `./op_meters_bar_tests ALL PASS (0 failures) exit 0`,
    sanitized `opd meters … --per-bar` / plain / usage runs exit 0/0/2 with 0 sanitizer reports.
    NOTE (pre-existing, not worked around): `CMAKE_BUILD_TYPE=Release` + `-DDZ_SANITIZE=ON` fails on
    THIS machine with gcc 13.3 `-Werror=array-bounds=` on the pre-existing `out.arrangement = {clip};`
    in `pattern_mode_project` (`src/engine/render.cpp:786`, WP-0192 code — untouched here).
    Reproduced identically on untouched base HEAD `d7e3fa3`
    (`builds/wp0243-base-asan/dz_build.log`, same single error), same finding as the WP-0193b handoff;
    the Debug configuration above is the CI-equivalent one.

### Metrics
- Files: 4 modified (+158/−13 total incl. task-file-free source diff `+145/−13` over the 4 modified
  files), 1 new test file (18 CHECKs), CMakeLists purely additive (+6 lines).
- Test suite: ctest 18 → 19 tests; `op_meters_bar_tests` runtime 0.52 s (0.5 s under ASAN too).
- No audio output affected: render paths untouched, all 16 goldens bit-identical; demo evidence is
  the JSON excerpts above (no WAV demo required).

### Notes / follow-ups
- `PROPOSE:` none — nothing outside scope needed to change.
- The untracked `wpdraft6/` dir in the checkout (nested git worktree of a draft agent, present before
  this package started) was left untouched and NOT committed: it is not part of this WP's file set.
- Follow-up idea (NOT done, out of scope): per-bar LUFS / a `--from-bar/--to-bar` slice of the
  `bars` array — both explicitly excluded here.
