---
id: WP-0210
title: `opd timeline` - per-bar ruler JSON (bar number, start beat, start sample, start seconds, tempo) via the renderer's own bar/beat mapping
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, cli:main, render, new:tests/test_timeline.cpp]
---

## Context - why this exists

Phase 1 transport/ruler work: the app's timeline and later the copilot both need "where does bar *i*
start?" as data — its beat, its sample position and the tempo in force there — without a GUI. This package
adds three small public functions to the renderer (`op::beats_at_bar`, `op::sample_at_beat`,
`op::bpm_at_beat`) plus an `opd timeline <project>` command that prints one canonical JSON row per bar.

The command does **no bar/beat/tempo math of its own**: every position comes from the renderer's existing
tempo map (`build_tempo_map` + `TempoMap::at()` in `src/engine/render.cpp`, currently file-private), so it
follows the tempo map today and the time signature automatically (WP-0191 already folds `time_signature`
into `Project::beats_per_bar`). It is read-only: it writes nothing and never modifies the project, and its
output is deterministic.

Existing code this package reuses (read it first, do not reimplement it): `build_tempo_map`, `TempoSeg`,
`TempoMap::at()`, `TempoMap::seg_index()`, `total_samples_for`, `sample_at_bar` in
`src/engine/render.{hpp,cpp}`; `op::Project`, `op::load_project`, `op::TimeSignature`,
`op::effective_time_signature` in `src/engine/model.hpp`.

## Spec - normative (this task file is the contract; `docs/spec/*` are NOT edited by this package)

### Files (exactly these 5; stage no others)
- `src/engine/render.hpp`, `src/engine/render.cpp` — additive API + two one-line delegations (below).
- `src/cli/main.cpp` — **purely additive** (no existing line modified or removed): one usage line, the new
  command, one dispatch line.
- `tests/test_timeline.cpp` (new) — own `main`, own `CHECK(cond, name)` macro printing `PASS <name>` /
  `FAIL <name>`, returns the failure count (copy the style of `tests/test_clip_trim.cpp`). Use a unique
  temp dir (`std::filesystem::temp_directory_path()` + a name containing the pid), never fixed `/tmp/op_*`
  paths, and remove it at the end. Do NOT edit `tests/test_main.cpp`.
- `CMakeLists.txt` — purely additive (do not change or delete any existing line):
  1. immediately after the block that ends with `add_test(NAME clip_trim COMMAND op_clip_trim_tests)` add
     exactly these lines (plus one blank line before them):
     ```cmake
     # WP-0210: opd timeline (bar ruler backend). Runs the opd binary, so it needs OPD_BIN and depends on it.
     add_executable(op_timeline_tests tests/test_timeline.cpp)
     target_link_libraries(op_timeline_tests PRIVATE opengine)
     target_compile_definitions(op_timeline_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
     add_dependencies(op_timeline_tests opd)
     add_test(NAME timeline COMMAND op_timeline_tests)
     ```
  2. directly after the line `list(APPEND DZ_OUR_TARGETS op_clip_trim_tests)  # WP-0193a` add
     `list(APPEND DZ_OUR_TARGETS op_timeline_tests)  # WP-0210`.

### Engine API (exactly these three declarations, in namespace `op`, in `src/engine/render.hpp`)
```cpp
double beats_at_bar(const Project& p, double bar);
long long sample_at_beat(const Project& p, double beat);
double bpm_at_beat(const Project& p, double beat);
```
- `beats_at_bar(p, bar)` returns `bar * static_cast<double>(p.beats_per_bar)`. This is the single
  definition of bar numbering (bars are 0-based, fractional bars allowed).
- `sample_at_beat(p, beat)` builds the renderer's own map with `build_tempo_map(p, p.sample_rate)` and
  returns `TempoMap::at(beat)` — i.e. for the segment containing `beat`
  (`seg_index` rule: last segment with `start_beats <= beat`) it returns
  `segment.start_sample + (long long)std::llround((beat - segment.start_beats) * segment.spb)`; with an
  empty `tempo_map` the legacy path `std::llround(beat * spb_const)`, `spb_const = sample_rate * 60.0 / bpm`.
  **No clamping**: unlike `sample_at_bar`, values past the end of the project are NOT capped at
  `total_samples_for(p)` (a ruler may draw beyond the last bar). Negative beats are not required.
- `bpm_at_beat(p, beat)` returns the tempo in BPM in effect at `beat`: with an empty `tempo_map` it returns
  `p.bpm`; otherwise the `bpm` of the segment that `TempoMap::seg_index(beat)` selects (the last tempo point
  whose `bar * beats_per_bar <= beat`). To get it, add a `double bpm = 0.0;` field to the file-private
  `struct TempoSeg` and set it in `build_tempo_map` from the tempo point that starts each segment
  (`p.tempo_map[i].bpm`, first segment from `p.tempo_map[0].bpm`). Do NOT derive it from `spb` (a round trip
  through samples-per-beat does not reproduce the stored bpm exactly).

### Existing behaviour must not change (bit-exactness)
- In `src/engine/render.cpp`: `total_samples_for` must compute its beat position as
  `beats_at_bar(p, p.length_bars)` and `sample_at_bar` must call `tm.at(beats_at_bar(p, bar))` instead of
  multiplying by `beats_per_bar` inline. These are the same expressions, so every existing render stays
  bit-identical (`harness/run_goldens.sh` must report the same fixtures, 0 failures). Do not change any
  other function's arithmetic; do not change any public signature or JSON key of existing commands.

### CLI — `opd timeline <project.opj> [--bars N]`
Add to `src/cli/main.cpp`, purely additively:
1. one usage line inserted directly AFTER the `opd steps <proj.opj> <pattern-index> [--steps N]` line:
   `"  opd timeline <project.opj> [--bars N]   (per-bar ruler: start beat, sample, seconds + tempo as JSON)\n"`
2. a `static std::string timeline_json(const op::Project& p, long long n_bars)` helper and a
   `static int cmd_timeline(int argc, char** argv)` placed immediately BEFORE `int main(`;
3. one dispatch line directly after the `if (cmd == "samples")` line:
   `if (cmd == "timeline") return cmd_timeline(argc - 1, argv + 1);`

Argument parsing (mirror `cmd_peaks`: options are checked before the file is touched): exactly one
positional argument; `--bars` takes a value parsed with the existing `parse_decimal(value, 4096, v)` helper.
Errors use the existing `fail(...)` helper and print NOTHING on stdout:
1. unknown option (any other argument starting with `-`, including a valueless `--bars`) or a second
   positional → `usage()` then `fail("usage", "unknown arg '<argument>'")` → exit 2;
2. no positional argument at all → `usage()` then `fail("usage", "timeline needs <project.opj>")` → exit 2;
3. `--bars` value that is not an integer in `[1, 4096]` (so `0`, `-1`, `1.5`, `abc`, empty) → `usage()` then
   `fail("usage", "bad --bars value '<argument>'")` → exit 2;
4. file does not exist → `fail("not_found", <path>)` → exit 3 (this check comes before parsing the project);
5. `op::load_project` fails → `fail("parse", err)` → exit 4 (message = the loader's error, unchanged);
6. after loading, let `B = (long long)std::ceil(p.length_bars)` (the number of bars; a final partial bar
   counts; `length_bars > 0` is enforced by the model so `B >= 1`). With no `--bars`, `n_bars = B`; with
   `--bars N > B` → `usage()` then
   `fail("usage", "--bars <N> is larger than the project's <B> bars")` (both numbers plain integers) → exit 2.
   (Cases 1-3 are all usage errors before the file is opened; which of them fires first for a command with
   several bad arguments does not matter, as long as it is exit 2 with empty stdout.)

On success: print `timeline_json(p, n_bars)` to stdout with `std::fwrite` and nothing else; exit 0.
**No bar/beat/tempo math in the CLI**: `timeline_json` and `cmd_timeline` must call
`op::beats_at_bar`, `op::sample_at_beat` and `op::bpm_at_beat` for every row value, and neither function may
contain the text `std::llround` or the text `60.0`. (Printing `p.beats_per_bar` in the header line is fine;
multiplying by it is not.)

### JSON (`timeline_json`, byte-exact — these bytes are the contract)
Example, `opd timeline tests/fixtures/goldens/tempo.opj --bars 5` (120 bpm with 72 bpm from bar 4, 4/4, 48 kHz):
```
{
  "sample_rate": 48000,
  "beats_per_bar": 4,
  "time_signature": "4/4",
  "length_bars": 8.000000,
  "bars": [
    {"bar": 0, "start_beat": 0.000000, "start_sample": 0, "start_seconds": 0.000000, "bpm": 120.00},
    {"bar": 1, "start_beat": 4.000000, "start_sample": 96000, "start_seconds": 2.000000, "bpm": 120.00},
    {"bar": 2, "start_beat": 8.000000, "start_sample": 192000, "start_seconds": 4.000000, "bpm": 120.00},
    {"bar": 3, "start_beat": 12.000000, "start_sample": 288000, "start_seconds": 6.000000, "bpm": 120.00},
    {"bar": 4, "start_beat": 16.000000, "start_sample": 384000, "start_seconds": 8.000000, "bpm": 72.00}
  ]
}
```
Rules (all line endings `\n`, one final `\n` after the closing `}`, no trailing spaces, no other whitespace
than shown):
- Top-level keys in exactly this order: `sample_rate`, `beats_per_bar`, `time_signature`, `length_bars`,
  `bars`; each on its own line indented two spaces; a comma after every one except `bars`.
- `sample_rate` = `p.sample_rate`, `beats_per_bar` = `p.beats_per_bar` as plain integers (`%d`).
- `time_signature` = a JSON string `"<num>/<den>"` from `op::effective_time_signature(p)` (so `"4/4"` for a
  project with no stored signature, `"3/4"`, and `"6/8"` for the 6/8 fixture — where `beats_per_bar` is 3
  because WP-0191 normalises bar length to quarter-note beats).
- `length_bars` = `p.length_bars` printed with `%.6f`.
- `"bars": [` opens the array on that line. Then one row per bar `i = 0 .. n_bars-1`, each on its own line
  indented four spaces, rows separated by `,\n`, and no comma after the last row. After the last row: `\n`,
  then the line `  ]`, then the line `}`.
- Row layout, exactly: `{"bar": <i>, "start_beat": <%.6f>, "start_sample": <lld>, "start_seconds": <%.6f>, "bpm": <%.2f>}`
  where `i` is the 0-based bar index (`%lld`), and
  - `start_beat` = `op::beats_at_bar(p, (double)i)`,
  - `start_sample` = `op::sample_at_beat(p, start_beat)` — in the PROJECT's sample rate (`%lld`),
  - `start_seconds` = `static_cast<double>(start_sample) / static_cast<double>(p.sample_rate)` printed with
    `%.6f` (this division is the only arithmetic allowed in the CLI),
  - `bpm` = `op::bpm_at_beat(p, start_beat)` printed with `%.2f`.
- Format every number with `std::snprintf`; no locale changes, no `std::fixed` streams. Values are exact —
  there is no tolerance to allow: the golden-project rows above must match byte for byte.

### Determinism / repo rules (ADR-0004)
No wall clock, no randomness, no fast-math, libm only via existing includes; `opd timeline` output must be
byte-identical run to run and across x86_64/aarch64. Read-only: the command opens the project for reading
only and writes nothing anywhere.

## Acceptance criteria
- [x] Everything above implemented; `CMakeLists.txt` and `src/cli/main.cpp` diffs are purely additive
      (`git diff --numstat` shows 0 deleted lines in both); existing goldens bit-identical
      (`bash harness/run_goldens.sh` → same fixture count, 0 failures).
- [x] `opd timeline tests/fixtures/goldens/tempo.opj` prints exactly 8 rows and the first five are the
      example above byte for byte; bar 5 is
      `{"bar": 5, "start_beat": 20.000000, "start_sample": 544000, "start_seconds": 11.333333, "bpm": 72.00}`,
      bar 6 `704000 / 14.666667`, bar 7 `864000 / 18.000000`. Two runs are byte-identical.
- [x] `opd timeline tests/fixtures/goldens/time_sig_3_4.opj` prints `"beats_per_bar": 3,` and
      `"time_signature": "3/4",`; bar 1 of that project is
      `{"bar": 1, "start_beat": 3.000000, "start_sample": 72000, "start_seconds": 1.500000, "bpm": 120.00},`.
      `opd timeline tests/fixtures/goldens/time_sig_6_8.opj` prints `"beats_per_bar": 3,` and
      `"time_signature": "6/8",`.
- [x] A project with fractional `length_bars` (e.g. `2.5`) gets a row for the final partial bar (`bar 2`)
      and no row beyond it; `--bars N` prints exactly the first N rows; `--bars` greater than that number is
      exit 2 with the specified message and empty stdout.
- [x] Engine API behaves as specified, including: `sample_at_beat(tempo.opj project, 36.0) == 1184000`
      (past the end, NOT clamped to `total_samples_for(p) == 1024000`) while `sample_at_bar(p, 99.0)` still
      returns `1024000`; `bpm_at_beat` is `120.0` at beat `15.75` and `72.0` at beat `16.0` for that project;
      with no tempo map `bpm_at_beat` returns the project bpm at any beat.
- [x] `tests/test_timeline.cpp` with at least 10 CHECKs covering: the three engine functions on a
      tempo-map project (segment boundary at bar 4, unclamped past-the-end vs `sample_at_bar` clamping) and
      on a default/3-beat project; `opd timeline` byte-exact JSON for the tempo-map project; one row per bar
      by default including a partial last bar; the 3/4 header and rows; exit codes 2 (usage, stdout empty),
      3 (missing file) and 4 (bad JSON); read-only (project file unchanged after a run).
- [x] `opd timeline` leaves the project file byte-identical and creates no files.
- [x] Standard gates: build (0 warnings under `-Werror`, `-j4`), `op_tests`, `op_timeline_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer
      build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_timeline_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `docs/` (no spec file for this package), golden fixtures
  (`tests/fixtures/`), `harness/`, `.github/`, `ai-sidecar/`, `src/app/`, `src/doc/`, `tests/test_main.cpp`.
- No other engine file than `src/engine/render.{hpp,cpp}`: not `model.*`, `ops.*`, `midi.*`, `wav.*`,
  `analyze.*`, `meters.*`, `dsp.hpp`, `sample_index.*`. No new engine module, no new CLI option beyond
  `--bars` (no `--json`, `--from-bar`, `--to-bar`, `--beats`, `--format`), no per-beat or per-step ruler
  subdivision, no bar labels/names, no MIDI export of the ruler, no changes to time-signature parsing or
  validation, no live transport / clock code.
- Do not change `sample_at_bar`'s clamping contract, `total_samples_for`'s rounding, or any existing error
  message; do not rename or move anything.

## Git
- Branch `wp-0210-opd-timeline-bar-beat` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0210: opd timeline - per-bar ruler JSON via the renderer's bar/beat mapping`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include `TASKS/WP-0210-opd-timeline-bar-beat.md`
  (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (5 files, exactly the ones in scope)

- **`src/engine/render.hpp`** (+17): the three public declarations `op::beats_at_bar`,
  `op::sample_at_beat`, `op::bpm_at_beat` with doc comments and usage examples. No existing declaration touched.
- **`src/engine/render.cpp`** (+27/-4): `double bpm = 0.0;` added to the file-private `struct TempoSeg`;
  `build_tempo_map` stores the starting tempo point's own `bpm` in every segment (first segment from
  `p.tempo_map[0].bpm`, never derived back from `spb`); the three new public functions defined after
  `sample_at_bar`; and the two mandated one-line delegations — `total_samples_for` now computes its beat
  position as `beats_at_bar(p, p.length_bars)` and `sample_at_bar` calls `tm.at(beats_at_bar(p, bar))`.
  Same expressions, so renders are bit-identical (see goldens below). No other function's arithmetic changed.
- **`src/cli/main.cpp`** (+86/-0, purely additive): one usage line after the `opd steps` line;
  `timeline_json()` + `cmd_timeline()` immediately before `int main(`; one dispatch line after
  `if (cmd == "samples")`. Argument parsing mirrors `cmd_peaks` (options validated before the file is touched);
  `--bars` parsed with the existing `parse_decimal(value, 4096, v)` and required `>= 1`; errors via `fail(...)`
  only. Every row value comes from `op::beats_at_bar` / `op::sample_at_beat` / `op::bpm_at_beat`; the only
  arithmetic in the CLI is the samples→seconds division. Neither new function contains `std::llround` or `60.0`:
  ```
  $ sed -n '/^static std::string timeline_json/,/^\/\/ opd timeline <project/p' src/cli/main.cpp | grep -cE 'std::llround|60\.0'
  0
  $ sed -n '/^static int cmd_timeline/,/^int main/p' src/cli/main.cpp | grep -cE 'std::llround|60\.0'
  0
  ```
- **`tests/test_timeline.cpp`** (new, 36 CHECKs): own `main`, own `CHECK` macro (test_clip_trim style),
  scratch dir `temp_directory_path()/op_timeline_tests_<pid>`, removed at the end. Covers the three engine
  functions on a tempo-map project (segment boundary at bar 4, unclamped past-the-end vs `sample_at_bar`
  clamping), on a constant-tempo project and on a 3-beat project; byte-exact CLI JSON (`--bars 5`, `--bars 3`);
  one row per bar incl. a partial last bar; the 3/4 header + rows and the 6/8 header; exit codes 2 (six usage
  variants, stdout empty in each), 3 and 4 with the exact `--bars > B` message; read-only (project bytes and
  directory listing unchanged).
- **`CMakeLists.txt`** (+8/-0, purely additive): the six mandated lines after the `clip_trim` block plus
  `list(APPEND DZ_OUR_TARGETS op_timeline_tests)  # WP-0210`.

Nothing else touched: no `docs/`, no fixtures, no `harness/`, no other engine file, no `tests/test_main.cpp`.

### How verified (commands + real output)

Purely additive diffs (`git diff --numstat`, add/delete):
```
8	0	CMakeLists.txt
86	0	src/cli/main.cpp
27	4	src/engine/render.cpp
17	0	src/engine/render.hpp
```

Criterion 2 — `opd timeline tests/fixtures/goldens/tempo.opj` (exit 0, exactly 8 rows):
```
$ ./build/opd timeline tests/fixtures/goldens/tempo.opj
{
  "sample_rate": 48000,
  "beats_per_bar": 4,
  "time_signature": "4/4",
  "length_bars": 8.000000,
  "bars": [
    {"bar": 0, "start_beat": 0.000000, "start_sample": 0, "start_seconds": 0.000000, "bpm": 120.00},
    {"bar": 1, "start_beat": 4.000000, "start_sample": 96000, "start_seconds": 2.000000, "bpm": 120.00},
    {"bar": 2, "start_beat": 8.000000, "start_sample": 192000, "start_seconds": 4.000000, "bpm": 120.00},
    {"bar": 3, "start_beat": 12.000000, "start_sample": 288000, "start_seconds": 6.000000, "bpm": 120.00},
    {"bar": 4, "start_beat": 16.000000, "start_sample": 384000, "start_seconds": 8.000000, "bpm": 72.00},
    {"bar": 5, "start_beat": 20.000000, "start_sample": 544000, "start_seconds": 11.333333, "bpm": 72.00},
    {"bar": 6, "start_beat": 24.000000, "start_sample": 704000, "start_seconds": 14.666667, "bpm": 72.00},
    {"bar": 7, "start_beat": 28.000000, "start_sample": 864000, "start_seconds": 18.000000, "bpm": 72.00}
  ]
}
$ ./build/opd timeline tests/fixtures/goldens/tempo.opj --bars 5 > bars5.json && cmp expected5.json bars5.json \
  && echo "OK: byte-identical to the task example"      # expected5.json = the contract block, typed verbatim
OK: byte-identical to the task example
$ ./build/opd timeline tests/fixtures/goldens/tempo.opj | md5sum; ./build/opd timeline tests/fixtures/goldens/tempo.opj | md5sum
167c4d72818d293dcb4c6d763c525230  -
167c4d72818d293dcb4c6d763c525230  -
```

Criterion 3 — time signatures:
```
$ ./build/opd timeline tests/fixtures/goldens/time_sig_3_4.opj | sed -n '2,4p;7,8p'
  "sample_rate": 48000,
  "beats_per_bar": 3,
  "time_signature": "3/4",
    {"bar": 0, "start_beat": 0.000000, "start_sample": 0, "start_seconds": 0.000000, "bpm": 120.00},
    {"bar": 1, "start_beat": 3.000000, "start_sample": 72000, "start_seconds": 1.500000, "bpm": 120.00},
$ ./build/opd timeline tests/fixtures/goldens/time_sig_6_8.opj | sed -n '1,6p'
{
  "sample_rate": 48000,
  "beats_per_bar": 3,
  "time_signature": "6/8",
  "length_bars": 2.000000,
  "bars": [
```

Criterion 4 — fractional `length_bars` (a 2.5-bar project): rows for bars 0..2 and nothing beyond;
`--bars 3` (= B) succeeds, `--bars 4` is exit 2 with empty stdout and the exact message:
```
    {"bar": 2, "start_beat": 8.000000, "start_sample": 192000, "start_seconds": 4.000000, "bpm": 120.00}
$ ./build/opd timeline half.opj --bars 2 | grep -c '"bar":'      → 2
$ out=$(./build/opd timeline half.opj --bars 4 2>err.txt); echo "stdout_len=${#out} exit=$?"; tail -1 err.txt
stdout_len=0 exit=2
error: usage: --bars 4 is larger than the project's 3 bars
```

Criterion 5 — engine API (from `op_timeline_tests`, all PASS): `sample_at_beat(tempo, 36.0) == 1184000`
unclamped while `total_samples_for(tempo) == 1024000` and `sample_at_bar(tempo, 99.0) == 1024000`;
`bpm_at_beat` 120.0 at beat 15.75 / 72.0 at beat 16.0; with no tempo map it returns the project bpm
(96.0) at beat 0 and at beat 12345.75, whose sample (370372500) is likewise not clamped.

Criterion 6 — new test binary:
```
$ ./build-wp0210/op_timeline_tests > tl.log; echo "exit=$?; PASS lines: $(grep -c '^PASS' tl.log); FAIL lines: $(grep -c '^FAIL' tl.log)"; tail -1 tl.log
exit=0; PASS lines: 36; FAIL lines: 0
ALL PASS
```

Criterion 7 — read-only (same run, project dir left exactly as found):
```
before=$(sha256sum half.opj); ls proj > ls_before
./build/opd timeline half.opj --bars 8
after=$(sha256sum half.opj);  ls proj > ls_after
OK: project file unchanged (sha256 659e55a8a8b998caa656bd25c53553192033a81f122fea0e7796737767592600)
OK: no files created in the project directory
```

Criterion 1 + 8 — standard gates (fresh `build-wp0210`, plus sanitizer tree `build-san-wp0210`):
```
### 1. fresh Release build (-Werror, -j4)
cmake --build exit=0; warning/error lines: 0; log lines: 61
### 2. ctest (all suites, fresh build)
100% tests passed, 0 tests failed out of 10        # unit loudness normalize lint peaks limiter choke meters clip_trim timeline
### 3. op_timeline_tests (fresh build)
exit=0; PASS lines: 36; FAIL lines: 0 → ALL PASS
### 4. harness/run_goldens.sh (fresh opd)
run_goldens: 15 fixtures, 0 failures                # identical to the pre-change baseline: same 15 fixtures, bit-exact
### 5. harness/test_cli_errors.sh (fresh opd)
test_cli_errors: 0 failures
### 6. harness/test_glue_scripts.sh (UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv)
test_glue_scripts: 0 failures
### 7. sanitizer build (-DDZ_SANITIZE=ON) of op_tests + op_timeline_tests
san build exit=0; warning/error lines: 0
100% tests passed, 0 tests failed out of 2          # unit (9.16 s) + timeline (0.27 s) under ASAN+UBSAN
```

### Metrics

- 5 files touched, all in scope; +138 / -4 lines repo-wide (the 4 deletions are the two mandated delegations
  and the two `push_back` lines that gained the `bpm` field).
- New CLI surface: 1 subcommand, 1 option (`--bars`, 1..4096), exit codes 0/2/3/4.
- 36 new CHECKs; total native test suites 9 → 10.

### Notes / follow-ups

- No audio output is affected: `render_project` / `render_stems` / `render_click` paths and their arithmetic are
  unchanged, and `harness/run_goldens.sh` reports the same 15 fixtures bit-exact as before the change — that is
  the before/after evidence in place of new WAVs.
- Cross-arch byte-identity (x86_64 vs aarch64) follows from the same deterministic math and `-ffp-contract=off`
  (ADR-0004); verified here on the linux-aarch64 reference rig only, run-to-run md5-identical. CI's x86_64 leg
  covers the other half.
- Per the package scope no `docs/spec/` file was written for `opd timeline`; when the ruler becomes a public
  contract for the app/copilot, a spec row is worth adding (ROADMAP row is the human's call).
- No `PROPOSE:` items — nothing outside scope needed changing.
