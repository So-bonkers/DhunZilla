---
id: WP-0199
title: `opd clips <project>` — read-only arrangement clip view (canonical JSON, renderer tempo-map geometry)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.6
human_review: none
touches: [render, cli:main, cmake, "new:tests/test_clips.cpp", spec:operations]
---

## Context — why this exists
Phase 1 exit criterion is "arrange a beat in the playlist". The future playlist panel and the copilot must
read arrangement state from ONE canonical source instead of parsing `.opj` JSON themselves. This package adds
the read-only view `opd clips <proj.opj>`: one JSON entry per arrangement clip, model order, with the stored
clip fields plus the clip's start/end in beats and samples computed with **the renderer's own** tempo-map
mapping (no re-derived math anywhere). It adds no model field, no operation, and changes no render — every
existing golden must stay bit-exact.

## Spec — normative
`docs/spec/operations.md` (new section "Arrangement clip view — `opd clips` — additive (WP-0199)") is
**already updated by the runner** before you start. Read it first; it is the contract and wins on any
difference. If `docs/spec/operations.md` does NOT contain that section, write BLOCKED in the handoff and stop;
do not invent the contract and do not edit `docs/spec/`. Summary (complete):

### CLI
```
opd clips <proj.opj>
```
Exactly one positional argument, no options. On success stdout is ONLY the JSON text below plus nothing else,
exit 0. Errors print NOTHING on stdout: missing/extra argument or any token starting with `-` → usage error,
exit 2, stderr line `error: usage: clips needs exactly <project.opj>`; file missing → exit 3 (`not_found`,
detail = the path); project fails to parse → exit 4 (`parse`, detail = the loader's own message). Read-only:
the command never writes files and never modifies the project. Deterministic: same input → byte-identical
stdout.

### Engine API (namespace `op`, in `src/engine/render.hpp`)
```cpp
std::string clips_json(const Project& p);
```
Pure function returning EXACTLY the text `opd clips` prints. Implementation lives in `src/engine/render.cpp`
(next to `pattern_mode_project`) because it must reuse the renderer's private `build_tempo_map`/`TempoMap::at`
and the public `op::pattern_extent` — never re-implement either.

### Output text (byte-exact contract)
A JSON array built with the vendored `nlohmann::json`, dumped as `arr.dump(2)` plus **exactly one trailing
`\n`** (two-space indent; keys sort alphabetically automatically). Empty/absent arrangement → exactly `[]\n`.
Doubles are serialized by nlohmann exactly as stored (shortest round-trip, integral-valued doubles keep `.0`);
`index`, `pattern`, `repeats`, `start_sample`, `end_sample` are JSON integers. Do not hand-format numbers.

Each entry has EXACTLY these keys and no others:
- `index` — int, the clip's position in `arrangement` (0-based).
- `pattern` — int, stored `clip.pattern`.
- `pattern_name` — string, the referenced pattern's `id`; **the key is OMITTED entirely when the pattern has
  no id** (never invented as `""`, `null`, or `"Pattern 0"`).
- `start_bar` — number, stored value. `repeats` — int, stored value verbatim (even when geometry clamps it).
- `gain_db`, `fade_in_beats`, `fade_out_beats` — numbers, stored values; ALWAYS present (defaults 0.0 print as
  `0.0`).
- `start_beats`, `end_beats`, `start_sample`, `end_sample` — see geometry.

The clip trim keys `offset_beats` / `length_beats` (WP-0193a) are NOT emitted and do NOT affect the geometry;
unknown keys in a project's clip objects were already dropped by the model parse and must never appear.

### Geometry (normative — mirror `for_each_placement` in `src/engine/render.cpp` exactly)
For each clip in `p.arrangement` order, with `pat = p.patterns[clip.pattern]`:
- `extent = op::pattern_extent(pat)` (explicit `length_beats` if present, else `max(note.t + note.dur)`, else 0).
- `start_beats = clip.start_bar * static_cast<double>(p.beats_per_bar)`.
- `end_beats = start_beats + static_cast<double>(clip.repeats) * extent` — a plain product with NO special case:
  an empty pattern has extent 0, so `end_beats == start_beats` and the two samples are equal for any `repeats`,
  while the entry still reports the stored `repeats`. No clamping/validation of `repeats` (a hand-built Project
  with `repeats` 0 gives `end == start`, negative gives `end < start`). The loader already guarantees
  `clip.pattern` is a valid index and `repeats` in [1, 4096]; do not add bounds handling for them.
- `start_sample`/`end_sample` = the renderer's own mapping at the PROJECT sample rate: build
  `TempoMap tmap = build_tempo_map(p, p.sample_rate)` once and return `tmap.at(start_beats)` / `tmap.at(end_beats)`
  (constant-bpm path `llround(beats * sr*60/bpm)`; piecewise segments accumulate with `llround` — project-model.md
  §Tempo map). **Unclamped**: a clip may extend past `length_bars`; report the true mapped position even when
  `end_sample > total_samples_for(p)`.

Byte-exact example (120 bpm, 4/4, 48 kHz → spb 24000; pattern `"Intro"` with notes spanning 8 beats; clip at
bar 1.5, repeats 2, gain −6.5, fades 0.5/1.0):
```json
[
  {
    "end_beats": 22.0,
    "end_sample": 528000,
    "fade_in_beats": 0.5,
    "fade_out_beats": 1.0,
    "gain_db": -6.5,
    "index": 0,
    "pattern": 0,
    "pattern_name": "Intro",
    "repeats": 2,
    "start_bar": 1.5,
    "start_beats": 6.0,
    "start_sample": 144000
  }
]
```

## Implementation guide (5 files; all edits purely ADDITIVE — modify/delete no existing line)
1. `src/engine/render.hpp` — add the `std::string clips_json(const Project& p);` declaration with a doc comment
   immediately BEFORE the closing `}  // namespace op`.
2. `src/engine/render.cpp` — add `#include <nlohmann/json.hpp>` (after the existing `#include <vector>`) and
   `#include <utility>` (after `<string>`); define `clips_json` immediately after `pattern_mode_project`
   (before `}  // namespace op`). Build `tmap` once before the loop.
3. `src/cli/main.cpp` — three additive insertions:
   - usage line, inserted immediately BEFORE the `opd steps` usage line, exactly:
     `        "  opd clips <proj.opj>   (arrangement clips as canonical JSON — read-only, model order)\n"`
   - a `static int cmd_clips(int argc, char** argv)` function placed immediately before the comment line that
     starts `// Parse a decimal argument` (style: copy `cmd_meters` — reject `argc != 2 || argv[1][0] == '-'`
     with `usage(); return fail("usage", "clips needs exactly <project.opj>");`, then `fs::exists` →
     `fail("not_found", path)`, then `op::load_project` → `fail("parse", err)`, then print
     `op::clips_json(p)` with `std::fwrite` and return 0);
   - dispatch line, inserted immediately BEFORE the `if (cmd == "steps")` line:
     `    if (cmd == "clips") return cmd_clips(argc - 1, argv + 1);`
4. `tests/test_clips.cpp` — NEW file (model structs are in `src/engine/model.hpp`: `Note{t_beats, dur_beats,
   midi_note, velocity}`, `Pattern{track, id, notes, length_beats}`, `Clip{pattern, start_bar, repeats, gain_db, ...}`;
   read them, do not guess field names), own `main`, its own small `CHECK(cond, name)` macro printing
   `PASS <name>` / `FAIL <name>` (copy the style from `tests/test_meters.cpp`), returns the failure count. Build
   Projects in code and call `op::clips_json` directly — **≥ 12 CHECKs** covering at least: empty arrangement →
   `"[]\n"`; one byte-exact canonical entry (the example above); unnamed pattern → no `pattern_name` key;
   model order preserved when clips are out of bar order; explicit pattern `length_beats` as the loop period;
   empty-pattern clip (`end == start`, stored `repeats` still reported); defaults printed as `0.0`; trim keys set
   on a clip → identical output to unset and no trim keys present; tempo-map piecewise samples; no clamping past
   `length_bars`; byte-identical repeat calls; fractional `start_bar` at a non-120 bpm. No file I/O in the tests.
5. `CMakeLists.txt` — immediately after the `add_test(NAME clip_trim COMMAND op_clip_trim_tests)` line add:
   ```cmake
   add_executable(op_clips_tests tests/test_clips.cpp)
   target_link_libraries(op_clips_tests PRIVATE opengine)
   add_test(NAME clips COMMAND op_clips_tests)
   ```
   and immediately after `list(APPEND DZ_OUR_TARGETS op_clip_trim_tests)  # WP-0193a` add
   `list(APPEND DZ_OUR_TARGETS op_clips_tests)  # WP-0199`.

## Acceptance criteria
- [ ] `opd clips <proj.opj>` on a project with clips prints exactly the spec'd JSON (model order, exact keys,
      trailing newline); empty/absent arrangement prints exactly `[]\n`; nothing else on stdout.
- [ ] Geometry: samples equal `TempoMap::at` values (hand-computed in your tests) under constant bpm AND a tempo
      map, including a clip starting exactly on a tempo-map breakpoint; unclamped past `length_bars`; empty
      pattern → `end == start`.
- [ ] Read-only & deterministic: project file bytes unchanged after running; two runs byte-identical; no files
      created.
- [ ] Errors: exit 2 (usage, stdout empty), 3 (missing file), 4 (parse error); no crash on any input.
- [ ] `tests/test_clips.cpp` with ≥ 12 CHECKs, all passing (`build/op_clips_tests` exits 0).
- [ ] All standard gates green: `-Werror` Release build with 0 warnings; `op_tests` ALL PASS;
      `bash harness/run_goldens.sh` (NO new golden, none modified); `bash harness/test_cli_errors.sh`;
      `bash harness/test_glue_scripts.sh`; sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` and
      `op_clips_tests` passes. Builds with `-j4` max.
- [ ] ADR-0004 respected: no clock/randomness/libm added (no `<chrono>`, `<random>`, `<cmath>`, `rand(` or `std::sin/cos/pow/exp/log` in the added lines); the view is a pure function of the model.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row), golden fixtures (`tests/fixtures/`, read-only, add none),
  `src/app/`, `ai-sidecar/`, `docs/adr/`, `docs/spec/*` (already updated by the runner), `src/doc/`,
  `tests/test_main.cpp`.
- `src/engine/model.*` and `src/engine/ops.cpp` — no model field, no operation, no diff change.
- Implicit legacy placements (empty arrangement synthesizing one clip per pattern) — print `[]\n`, invent nothing.
- Reporting or honouring clip trim (`offset_beats`/`length_beats`) in this view; fade sample bounds; sorting or
  filtering options (`--track`, `--from-bar`, `--json` flag); MIDI export changes.

## Git
- Branch `wp-0199-opd-clips-arrangement-clip` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0199: opd clips — read-only arrangement clip view (canonical JSON + renderer tempo-map geometry)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:** (all edits purely additive — `git diff --numstat` shows 0 deleted lines in every source file)
  - `src/engine/render.hpp` +10: declared `std::string clips_json(const Project& p);` with a doc comment, immediately before `}  // namespace op`.
  - `src/engine/render.cpp` +37: `#include <utility>` (after `<string>`) and `#include <nlohmann/json.hpp>` (after `<vector>`); defined `clips_json` right after `pattern_mode_project`. It reuses the renderer's private `build_tempo_map` / `TempoMap::at` (built ONCE before the loop, at `p.sample_rate`) and the public `op::pattern_extent` — no geometry is re-derived. Keys are emitted into an `nlohmann::json` object (`dump(2) + "\n"`), so number formatting is nlohmann's, never hand-formatted; `pattern_name` is inserted only when `pat.id` is non-empty; trim keys are never touched.
  - `src/cli/main.cpp` +21: the usage line before `opd steps`; `static int cmd_clips(int argc, char** argv)` (copy of `cmd_meters`'s shape) immediately before `// Parse a decimal argument`; dispatch line before `if (cmd == "steps")`.
  - `tests/test_clips.cpp` NEW: own `main`, own `CHECK` macro (style copied from `tests/test_meters.cpp`), **19 CHECKs**, no file I/O — every Project is built in code.
  - `CMakeLists.txt` +5: `op_clips_tests` executable + `add_test(NAME clips …)` after the `clip_trim` block, and `list(APPEND DZ_OUR_TARGETS op_clips_tests)  # WP-0199`.
  - `docs/spec/operations.md`: the runner-installed "Arrangement clip view — `opd clips`" section, committed **unchanged** (read first; it was the contract). No other `docs/` touched. No model field, no op, no diff change, no golden added or modified.

- **How verified:** (g++ 13.3.0, cmake 3.28.3, aarch64; builds `-j4`)
  - **`opd clips <proj.opj>` prints exactly the spec'd JSON, model order, trailing newline; empty arrangement → `[]\n`; nothing else on stdout.**
    ```
    $ ./build/opd clips tests/fixtures/goldens/arranged.opj      # 5 clips, bpm 96 → spb 30000, no pattern ids
    exit=0
    [
      {
        "end_beats": 3.0,
        "end_sample": 90000,
        "fade_in_beats": 0.0,
        "fade_out_beats": 0.0,
        "gain_db": 0.0,
        "index": 0,
        "pattern": 0,
        "repeats": 1,
        "start_bar": 0.0,
        "start_beats": 0.0,
        "start_sample": 0
      },
      … (entries index 1..4 follow in model order; entry 3 = pattern 2 repeats 4 → end_beats 16.0 / end_sample 480000)
    ]
    PASS read_only_project_bytes_unchanged (a1c8b260e8eecf8a02d5c907961ccbbbceaa53b3fbe4f178f36aee94f0e141f1)
    PASS no_files_created_in_fixture_dir (31 entries)

    # absent "arrangement" key, two patterns present → implicit legacy placements NOT synthesized:
    $ ./build/opd clips empty.opj ; exit=0
    0000000   [   ]  \n
    0000003
    ```
    The byte-exact spec example (pattern "Intro" spanning 8 beats, bar 1.5, repeats 2, gain −6.5, fades 0.5/1.0) is asserted verbatim in `clips.canonical_entry_byte_exact` — see the test run below.
  - **Geometry = `TempoMap::at`, hand-computed; constant bpm AND tempo map incl. a clip starting exactly on a breakpoint; unclamped past `length_bars`; empty pattern → end == start.** All in `build/op_clips_tests`:
    ```
    $ ./build/op_clips_tests ; exit=0
    PASS clips.empty_arrangement_prints_empty_array_even_with_patterns
    PASS clips.canonical_entry_byte_exact
    PASS clips.exactly_one_trailing_newline
    PASS clips.unnamed_pattern_omits_pattern_name_key
    PASS clips.unnamed_pattern_still_reports_index_and_geometry
    PASS clips.model_order_not_sorted_by_bar
    PASS clips.index_keys_follow_model_order
    PASS clips.explicit_length_beats_is_the_loop_period
    PASS clips.empty_pattern_end_equals_start
    PASS clips.empty_pattern_reports_stored_repeats
    PASS clips.defaults_print_as_zero_point_zero
    PASS clips.trim_keys_do_not_change_output
    PASS clips.trim_keys_absent_from_output
    PASS clips.tempo_map_piecewise_byte_exact
    PASS clips.tempo_map_total_samples_sanity
    PASS clips.repeat_calls_byte_identical
    PASS clips.unclamped_end_past_length_bars
    PASS clips.end_sample_may_exceed_total_samples_for
    PASS clips.fractional_start_bar_non_120_bpm
    ALL PASS (0 failures)
    ```
    Hand-computed values pinned byte-exactly: 120 bpm/48 kHz → spb 24000 (bar 1.5 → beats 6.0 → sample 144000; end beats 22.0 → 528000); tempo map {bar 0 @120, bar 2 @60} → segment 0 accumulates `llround(8 × 24000) = 192000`, so a clip at bar 1.0 maps beats [4, 8) → samples [96000, 192000] and the **breakpoint** clip at bar 2.0 maps beats [8, 12) → `192000 + llround(4 × 48000)` = [192000, 384000]; unclamped case `length_bars` 2.0 (total 192000) with end beats 38.0 → `end_sample` 912000; empty pattern extent 0.0 → `end_beats == start_beats` and equal samples while `"repeats": 7` is still reported; 96 bpm → spb 30000 with fractional bar 1.25 → beats 5.0 → sample 150000.
  - **Read-only & deterministic:** `sha256sum` of the fixture identical before/after (hash above), fixture dir entry count unchanged, two runs `cmp`-identical:
    ```
    PASS two_runs_byte_identical
    ```
    plus `clips.repeat_calls_byte_identical` at engine level.
  - **Errors (exit codes + empty stdout).** `ck()` asserts exit code AND that stdout is empty:
    ```
    PASS err_no_args (exit 2, stdout empty)
    PASS err_extra_arg (exit 2, stdout empty)      # clips <proj> extra.opj
    PASS err_option_token (exit 2, stdout empty)   # clips --json <proj>
    PASS err_parse (exit 4, stdout empty)          # "this is not json {{{"
    PASS err_missing (exit 3, stdout empty)        # nope.opj
    usage stderr: error: usage: clips needs exactly <project.opj>
    missing stderr: error: not_found: /tmp/tmp.qLNi84Mm2d/nope.opj
    parse stderr: error: parse: json: [json.exception.parse_error.101] parse error at line 1, column 2: …
    ```
    Further input probes, all clean exits (no crash): random binary `exit=4`, empty file `exit=4`, wrong-typed JSON (`"bpm":"fast"`) `exit=4`, top-level array `exit=4`, unreadable file (mode 000) `exit=4` (`cannot open …`), `-`/`--`/`-x` `exit=2`, non-existent name `exit=3`.
  - **`tests/test_clips.cpp` ≥ 12 CHECKs, all passing:** 19 CHECKs, `ALL PASS (0 failures)`, exit 0 (run above); also green under sanitizers and registered with CTest:
    ```
    $ ctest --test-dir build -R clips --output-on-failure
    1/1 Test #10: clips ............................   Passed    0.00 sec
    100% tests passed, 0 tests failed out of 1
    ```
  - **Standard gates.** Fresh out-of-tree Release build (`-Werror` is ON by default for our targets), 0 warnings:
    ```
    $ cmake -S . -B …/build-rel-clean -DCMAKE_BUILD_TYPE=Release && cmake --build …/build-rel-clean -j4
    clean release build exit=0
    warning lines: 0
    error lines: 0
    ```
    `./build/op_tests` → `ALL PASS (0 failures)`, exit 0 (1074 PASS lines). Goldens, **no new golden added, none modified** (`git status --porcelain tests/fixtures` empty):
    ```
    $ bash harness/run_goldens.sh            → goldens exit=0
      PASS sampler_repitch / sampler_trim / synth_v1 / tempo / time_sig_3_4 / time_sig_6_8 … 
      PASS negative-control (corrupt golden rejected)
      run_goldens: 15 fixtures, 0 failures
    $ bash harness/test_cli_errors.sh        → cli_errors exit=0   (84 PASS lines)
      test_cli_errors: 0 failures
    $ export UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv
    $ (cd ai-sidecar && uv sync)             → uvsync exit=0 ; bash harness/test_glue_scripts.sh → glue exit=0
      test_glue_scripts: 0 failures
    ```
    Sanitizers (`-DDZ_SANITIZE=ON`, Debug, ASAN+UBSAN): build exit 0 with 0 warning lines, and both suites green —
    ```
    $ ./build-san/op_clips_tests → ALL PASS (0 failures)   exit=0
    $ ./build-san/op_tests       → ALL PASS (0 failures)   exit=0
    $ bash harness/test_cli_errors.sh ./build-san/opd → test_cli_errors: 0 failures
    ```
  - **ADR-0004 respected.** Added lines only, grepped for banned tokens — none present (`grep -nE '<chrono>|<random>|<cmath>|rand\(|std::sin|std::cos|std::pow|std::exp|std::log'` over the diff's `+` lines → exit 1, no match). No new libm call: the view calls `pattern_extent` and `TempoMap::at`, which already own the only `llround`.
- **Metrics:** +73 lines across 5 files (render.hpp 10, render.cpp 37, main.cpp 21, CMakeLists.txt 5) plus a 309-line new test file; 0 deletions; 19 new CHECKs; 15 goldens unchanged and still bit-exact; 84 CLI contract checks + glue e2e still green.
- **PROPOSE / BLOCKED:**
  - `PROPOSE:` (pre-existing, NOT introduced here, out of this WP's scope) — passing a **directory** path to any project-loading subcommand aborts with an uncaught `std::__ios_failure` (`basic_filebuf::underflow … Is a directory`, exit 134 / core dump): `./build/opd clips /`, and identically `meters /`, `lint /`, `info /`. The gap lives in `op::load_project`'s file read (`src/engine/model.cpp`, explicitly out of scope for WP-0199, which also forbids touching `model.*`) — every command that does `fs::exists` + `load_project` inherits it. A follow-up WP should make the loader return `false` + a `parse`/IO error string (or wrap the read) so all such paths exit 4 instead of aborting; fixing it for `clips` alone would silently diverge from its siblings, so I kept the spec'd behaviour (`fs::exists` → not_found, then `load_project` → parse) verbatim.
  - Note: no clang on this rig (AGENTS.md "no clang locally"), so the clang `-Werror` leg runs in CI only; gcc 13.3.0 is clean with 0 warnings. No demo artifact attached — audio output is untouched by design (every golden stayed bit-exact).

