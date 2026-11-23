---
id: WP-0228
title: `opd patterns <project>` — read-only pattern-bar listing as canonical JSON (effective length via the renderer's own `pattern_extent`, swing, note counts, using clip indices)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.6
human_review: none
touches: [cmake, cli:main, new:src/engine/patterns.hpp, new:src/engine/patterns.cpp, new:tests/test_patterns.cpp]
---

## Context - why this exists
Phase 1 exit criterion is "arrange a beat in the playlist"; the pattern bar (and the future copilot) must be
able to read every pattern of a project — which channel it belongs to, how long it actually loops, how swung it
is, how full it is and whether the arrangement uses it at all — from ONE canonical source instead of parsing
`.opj` JSON themselves. This package adds the read-only view `opd patterns <proj.opj>`: one JSON entry per
pattern **in model order**, whose effective length comes from **the renderer's own** `op::pattern_extent` (so an
explicit `length_beats` and a note-derived one are never re-derived differently here), plus the indices of the
arrangement clips that place it. It adds no model field, no operation and changes no render: every existing
golden must stay bit-exact, and the command writes nothing and modifies nothing.

Existing code this package reuses (read it first, do NOT reimplement or modify it): `op::pattern_extent`
(`src/engine/model.hpp`, implemented in `src/engine/model.cpp` — the single source of truth for a pattern's loop
period: `*length_beats` when present, else `max(note.t + note.dur)` over its notes, `0.0` with no notes),
`op::Project` / `op::Pattern` / `op::Clip` / `op::Track` / `op::load_project` (`src/engine/model.hpp`). The CLI
shape to copy is `cmd_clips` (WP-0199) and the engine-module shape to copy is `op::meters`
(`src/engine/meters.{hpp,cpp}` + its one `target_sources` line).

## Spec - normative (this task file is the contract)

### Files (exactly these 5; stage no others)
- `src/engine/patterns.hpp` (new) - declares `op::patterns_json` with a doc comment stating the contract below.
- `src/engine/patterns.cpp` (new) - the implementation.
- `tests/test_patterns.cpp` (new) - own `main`, own `CHECK(cond, name)` printing `PASS <name>` / `FAIL <name>`
  (copy the style of `tests/test_meters.cpp`), returns the failure count; project files it needs go in one
  per-process scratch dir (`std::filesystem::temp_directory_path()` + a name containing the pid), removed at the
  end — never fixed `/tmp/op_*` paths. Do NOT edit `tests/test_main.cpp`.
- `src/cli/main.cpp` - four purely ADDITIVE insertions (include line, usage line, `static int cmd_patterns(...)`,
  dispatch line); modify/delete no existing line.
- `CMakeLists.txt` - purely additive: the exact lines and anchors below.

### Interface (exact signatures, CLI syntax, JSON keys, error strings, exit codes)

#### CLI
```
opd patterns <proj.opj>
```
Exactly one positional argument, no options. On success stdout is ONLY the JSON text described below and nothing
else, exit 0. Errors print NOTHING on stdout: missing/extra argument or any token starting with `-` → usage
error, exit 2, stderr line `error: usage: patterns needs exactly <project.opj>`; file missing → exit 3
(`not_found`, detail = the path); project fails to parse → exit 4 (`parse`, detail = the loader's own message).
Read-only: never writes files, never modifies the project. Deterministic: same input file → byte-identical stdout.

#### Engine API (namespace `op`, in NEW `src/engine/patterns.hpp`)
```cpp
std::string patterns_json(const Project& p);
```
Pure function returning EXACTLY the text `opd patterns` prints. No I/O, no clock, no randomness; two calls on the
same `Project` return identical bytes. A `Project` with no patterns returns exactly `"[]\n"`. Caller-validated
invariant (the loader already enforces it): `0 <= pattern.track < p.tracks.size()`, so `track_name` always resolves.

#### Output text (byte-exact contract)
A JSON array built with the vendored `nlohmann::json`, dumped as `arr.dump(2)` plus **exactly one trailing `\n`**
(two-space indent; object keys sort alphabetically automatically). Do NOT hand-format any number: `index`,
`notes`, `track` and every element of `clips` are JSON integers; `length_beats` and `swing` are the model's double
values, so their decimal text is nlohmann's shortest round-trip (a project whose notes derive a length of 4.9
prints `"length_beats": 4.9`; defaults print as `0.0`, never `0`).

One entry per pattern **in `Project::patterns` order — never sorted, never filtered** (empty patterns are listed
too). Seven keys are ALWAYS present in every entry; `id` is the only optional one:

| key | value |
|---|---|
| `index` | int, the pattern's position in `Project::patterns` (0-based) |
| `track` | int, stored `pattern.track` (the channel this pattern plays on) |
| `track_name` | string, `p.tracks[pattern.track].name` verbatim (JSON-escaped by nlohmann) |
| `id` | string, stored `pattern.id` — **present ONLY when it is non-empty; omitted entirely otherwise** (never `""`, never `null`, never a fallback like `"Pattern 0"`) |
| `length_beats` | number = `op::pattern_extent(pattern)` — CALL that function, do not re-derive: explicit `length_beats` wins even when it is shorter or longer than the notes imply; otherwise `max(t + dur)` over the notes (`0.0` when there are none) |
| `swing` | number, stored `pattern.swing`; always present (default prints `0.0`) |
| `notes` | int, the number of notes in THIS pattern (`pattern.notes.size()`) — a plain count, no per-track grouping, no note details |
| `clips` | array of ints, ascending: every index `i` into `Project::arrangement` whose clip has `clip.pattern == index`, scanned in arrangement order; `[]` when the pattern is unused **or when the project has no/empty `arrangement`** (the implicit legacy "every pattern once at bar 0" placements are NEVER synthesized) |

No other keys — no `bpm`, no `name`, no `pattern`, no `repeats`, no `start_bar`, no `length_source`, no
`notes_per_track`, no wrapper object: stdout is ONLY the array.

Byte-exact example (two tracks; pattern 0 unnamed on track 0 with derived length 1.5 + 0.25 = 1.75; pattern 1
named `groove` on track 1, swing 0.25, an EXPLICIT `length_beats` of 2.0 that is shorter than its notes would
give; three clips so pattern 0 is used by clip 1 only and pattern 1 by clips 0 and 2):

```json
[
  {
    "clips": [
      1
    ],
    "index": 0,
    "length_beats": 1.75,
    "notes": 2,
    "swing": 0.0,
    "track": 0,
    "track_name": "Zeta Kick"
  },
  {
    "clips": [
      0,
      2
    ],
    "id": "groove",
    "index": 1,
    "length_beats": 2.0,
    "notes": 1,
    "swing": 0.25,
    "track": 1,
    "track_name": "Alpha Bass"
  }
]
```

### Implementation guide (all edits purely ADDITIVE — modify/delete no existing line)
1. `src/engine/patterns.hpp` (new): `#pragma once`, include `<string>` and `"engine/model.hpp"`, declare
   `std::string patterns_json(const Project& p);` inside `namespace op` with a doc comment stating the contract
   above (pure, `dump(2)` + `\n`, seven always-present keys plus optional `id`, clips = arrangement indices,
   legacy projects report `[]`, empty patterns → `"[]\n"`).
2. `src/engine/patterns.cpp` (new): include its own header, `<cstddef>`, `<string>`, `<utility>` and
   `<nlohmann/json.hpp>`. For each `i` in `[0, p.patterns.size())`: scan `p.arrangement` once collecting the
   indices whose `pattern == static_cast<int>(i)` into a JSON array; then fill an `nlohmann::json` object with
   `e["index"]`, `e["track"]`, `e["track_name"] = p.tracks[...].name`, `if (!pat.id.empty()) e["id"] = pat.id;`,
   `e["length_beats"] = pattern_extent(pat);`, `e["swing"]` and `e["notes"]`; push into an array and return
   `arr.dump(2) + "\n"`.
3. `src/cli/main.cpp` — four additive insertions:
   - include line, immediately AFTER `#include "engine/ops.hpp"` (keeps the block alphabetical):
     `#include "engine/patterns.hpp"  // WP-0228`
   - usage line, inserted immediately AFTER the `opd clips` usage line and immediately BEFORE the `opd steps`
     usage line, exactly:
     `        "  opd patterns <proj.opj>   (pattern-bar listing as canonical JSON — read-only, model order)\n"`
   - a `static int cmd_patterns(int argc, char** argv)` function placed immediately AFTER `cmd_clips` and
     immediately BEFORE the comment line that starts `// Parse a decimal argument` (copy `cmd_clips` verbatim in
     shape: reject `argc != 2 || argv[1][0] == '-'` with `usage(); return fail("usage", "patterns needs exactly <project.opj>");`,
     then `fs::exists` → `fail("not_found", path)`, then `op::load_project` → `fail("parse", err)`, then print
     `op::patterns_json(p)` with `std::fwrite` and return 0);
   - dispatch line, inserted immediately AFTER `if (cmd == "clips") return cmd_clips(argc - 1, argv + 1);`:
     `    if (cmd == "patterns") return cmd_patterns(argc - 1, argv + 1);`
4. `tests/test_patterns.cpp` — NEW file, own `main`, own `CHECK` macro (style of `tests/test_meters.cpp`),
   **≥ 25 CHECKs**, Projects built in code plus at least one save/load round through a scratch-dir `.opj` and the
   real binary via `OPD_BIN` for the CLI checks. Cover at least: empty `Project` → exactly `"[]\n"`; the
   byte-exact example above from both a hand-built Project and the loaded file; exactly one trailing `\n`; model
   order preserved with deliberately non-alphabetical names/ids; `index` values `0..n-1`; `id` present when set
   and ABSENT (never `""`/`null`) when not; explicit `length_beats` wins both shorter AND longer than the notes;
   derived length = `max(t + dur)` with the notes given OUT OF ORDER; a pattern with no notes → `"length_beats": 0.0`
   and `"notes": 0`; the reported length follows `op::pattern_extent` when the model changes; `swing` default
   prints `0.0` and a set value passes through (including `0.75`); note counts exact; `clips` ascending and
   complete for a pattern used by non-adjacent clips, `[]` for an unused one; a legacy project (no `arrangement`
   key) → every entry `"clips": []`; clip parameters (`repeats`, `gain_db`, fades, `offset_beats`, trim
   `length_beats`) do NOT change the output at all; `track`/`track_name` follow the pattern's own track when
   patterns are not in track order; track-name JSON escaping (quote + backslash); only the seven/eight keys
   appear (no invented extras); two calls byte-identical; CLI: exit 0 with stdout == `patterns_json(project)`,
   project file bytes unchanged, two runs byte-identical, no files created; CLI errors: no argument / extra
   argument / `--json` → exit 2 with empty stdout and stderr containing `patterns needs exactly <project.opj>`,
   missing file → exit 3, garbage file → exit 4.
5. `CMakeLists.txt` — immediately after the line `target_sources(opengine PRIVATE src/engine/meters.cpp)  # WP-0194` add:
   ```cmake
   target_sources(opengine PRIVATE src/engine/patterns.cpp)  # WP-0228
   ```
   immediately after `add_test(NAME timeline COMMAND op_timeline_tests)` add:
   ```cmake

   # WP-0228: opd patterns — pattern-bar listing. Runs the opd binary, so it needs OPD_BIN and depends on it.
   add_executable(op_patterns_tests tests/test_patterns.cpp)
   target_link_libraries(op_patterns_tests PRIVATE opengine)
   target_compile_definitions(op_patterns_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
   add_dependencies(op_patterns_tests opd)
   add_test(NAME patterns COMMAND op_patterns_tests)
   ```
   and immediately after `list(APPEND DZ_OUR_TARGETS op_clips_tests)  # WP-0199` add
   `list(APPEND DZ_OUR_TARGETS op_patterns_tests)  # WP-0228`.

### Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, libm only via dsp.hpp, no fast-math; passes `-Werror` and
`-DDZ_SANITIZE=ON`. The added lines must contain no `<chrono>`, `<random>`, `<cmath>`, `rand(` or
`std::sin/cos/pow/exp/log` — the view is a pure function of the model.

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`, NO golden
      added or modified).
- [x] `opd patterns tests/fixtures/goldens/demo.opj` (a LEGACY project with no `arrangement`) prints exactly 3
      entries in model order — `"Metronome"` (`length_beats` 7.25, `notes` 8), `"Bassline"` (4.9, 6) and `"Kick"`
      (11.0, 8) — every entry with `"clips": []`, `"swing": 0.0` and no `id` key; nothing else on stdout; exit 0.
- [x] `opd patterns tests/fixtures/goldens/arranged.opj` reports 4 entries whose `clips` arrays are `[0, 2]`,
      `[1]`, `[3]` and `[4]` in pattern order (ascending arrangement indices, non-adjacent included).
- [x] `opd patterns tests/fixtures/goldens/clip_trim.opj` shows `"swing": 0.25` on pattern 0 and `"swing": 0.0`
      on pattern 1; `opd patterns tests/fixtures/goldens/tempo.opj` shows an UNUSED pattern with `"clips": []`
      next to a used one with `"clips": [0]`.
- [x] A project whose pattern has BOTH an explicit `length_beats` and notes reports the explicit number, whether
      it is shorter or longer than `max(t + dur)`; without `length_beats` the reported number equals
      `op::pattern_extent(pattern)` for that pattern.
- [x] Read-only & deterministic: project file bytes unchanged after running; two runs byte-identical; no files
      created anywhere; the command never mutates the model.
- [x] Errors: exit 2 (usage, stdout empty, stderr contains `patterns needs exactly <project.opj>`), 3 (missing
      file), 4 (parse error); no crash on any input.
- [x] `tests/test_patterns.cpp` with ≥ 25 CHECKs, all passing (`build/op_patterns_tests` exits 0).
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_patterns_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_patterns_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds the row), golden fixtures (`tests/fixtures/`, read-only, add none),
  `harness/`, `.github/`, `ai-sidecar/`, `docs/` (including `docs/spec/` — this package changes no existing
  behaviour and needs no spec edit; the task file is the contract), `docs/adr/`, `src/app`, `src/doc`,
  `tests/test_main.cpp`.
- `src/engine/model.*`, `src/engine/render.*`, `src/engine/ops.cpp`, `src/engine/midi.cpp`,
  `src/engine/lint.cpp` — no model field, no operation, no render/MIDI/lint change. Read them; modify none.
  In particular do NOT copy the body of `op::pattern_extent`: call it.
- Any project-level keys in the output (`bpm`, `length_bars`, track counts, a wrapper object), any clip details
  beyond the indices (`start_bar`, `repeats`, gains, fades, trims), and any note contents: the output is ONLY the
  array of per-pattern entries described above.
- Sorting, filtering, pagination or options (`--sort`, `--track`, `--json`, `--all`): none. Model order only.
- Synthesizing implicit legacy placements (an absent `arrangement` means NO clips here, even though the renderer
  plays every pattern once at bar 0), and inventing names/ids for unnamed patterns.

## Git
- Branch `wp-0228-opd-patterns-pattern-listing` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0228: opd patterns — read-only pattern-bar listing as canonical JSON (effective length via the renderer's own pattern_extent, swing, note counts, using clip indices)`. Stage explicit paths only (never `git add -A`). Include `TASKS/WP-0228-opd-patterns-pattern-listing.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the 5 files, all edits purely additive)
- `src/engine/patterns.hpp` (new): declares `std::string patterns_json(const Project& p);` in `namespace op` with a doc comment stating the full contract (pure, `dump(2)` + one `\n`, seven always-present keys plus optional `id`, clips = arrangement indices, legacy → `[]`, no patterns → `"[]\n"`), usage example included.
- `src/engine/patterns.cpp` (new): builds the array with the vendored nlohmann::json; per pattern scans `p.arrangement` once for `clip.pattern == i`, fills `index/track/track_name/(id)/length_beats/swing/notes/clips` and returns `arr.dump(2) + "\n"`. The effective length is a direct call to `op::pattern_extent(pat)` — never re-derived. No I/O, clock, randomness or libm; forbidden tokens (`<chrono>`, `<random>`, `<cmath>`, `rand(`, `std::sin/cos/pow/exp/log`) grep clean in all three new files (exit 1 = none found).
- `src/cli/main.cpp`: four additive insertions only — include after `engine/ops.hpp` (alphabetical), the exact usage line between `opd clips` and `opd steps`, `static int cmd_patterns(...)` between `cmd_clips` and the `// Parse a decimal argument` comment (shape copied from `cmd_clips` verbatim), dispatch line after `clips`. `git diff --stat`: **31 insertions, 0 deletions** across main.cpp + CMakeLists.txt.
- `CMakeLists.txt`: `target_sources(opengine PRIVATE src/engine/patterns.cpp)  # WP-0228`, the OPD_BIN test block (`op_patterns_tests` + `add_test(NAME patterns ...)`), and `list(APPEND DZ_OUR_TARGETS op_patterns_tests)  # WP-0228` — at the specified anchors.
- `tests/test_patterns.cpp` (new): own main, own CHECK (test_meters style), **41 CHECKs**, scratch dir `temp_directory_path()/op_patterns_tests_wp0228_<pid>` removed at end; covers every bullet of the implementation-guide coverage list including CLI via `OPD_BIN`.

### Verification — every acceptance criterion with real output
All builds via `dz_build.sh` (skill), build dir `/home/shubhankar_dgx/qwen-scratch/build-wp0228` (Release, `-Werror`, `-j4`).

1. **Build 0 warnings + unit tests:**
```
$ grep -ci "warning" build-wp0228/dz_build.log
0
$ build-wp0228/op_tests | tail -3
PASS pmode.err_no_length_unnamed
PASS pmode.failure_leaves_out_untouched
ALL PASS (0 failures)      # op_tests exit=0
```
2. **`tests/test_patterns.cpp` ≥ 25 CHECKs, all passing** — 41 CHECKs:
```
$ build-wp0228/op_patterns_tests | tail -1
ALL PASS (0 failures)      # exit=0; ctest -R "^patterns$" → "100% tests passed, 0 tests failed out of 1"
```
3. **Legacy demo.opj** — exactly the 3 entries in model order, `"clips": []`, `"swing": 0.0`, no `id` key, nothing else on stdout, exit 0 (full stdout shown in this session): `Metronome 7.25/8`, `Bassline 4.9/6`, `Kick 11.0/8`.
```
$ build-wp0228/opd patterns tests/fixtures/goldens/demo.opj | grep -c '"id"'   → 0
exit=0
```
4. **arranged.opj** — `entries: 4`, `clips per pattern: [[0, 2], [1], [3], [4]]` (ascending arrangement indices, non-adjacent included).
5. **clip_trim.opj** — pattern 0 `"swing": 0.25`, pattern 1 `"swing": 0.0`. **tempo.opj** — `[(0, []), (1, [0])]` (unused next to used).
6. **Explicit vs derived length** — covered by `patterns.explicit_shorter_than_notes_wins` (explicit 2.0 beats the notes' 3.0), `patterns.explicit_longer_than_notes_wins` (8.0 beats 1.5) and `patterns.derived_length_out_of_order_notes` (out-of-order notes → 3.5 = max(t+dur)); all PASS above.
7. **Read-only & deterministic:**
```
exit codes: 0 0
project bytes UNCHANGED          # md5 before/after identical
two runs byte-identical          # cmp run1.json run2.json
no files created by the command  # find-snapshot of the dir unchanged
```
Plus engine-side `patterns.cli_project_file_bytes_unchanged`, `patterns.cli_two_runs_byte_identical`, `patterns.cli_creates_no_files`, `patterns.two_calls_byte_identical` PASS.
8. **Errors** (stdout empty in every case — `wc -c` = 0):
```
no-arg exit=2      extra-arg exit=2   --json exit=2   # stderr: patterns needs exactly <project.opj>
missing-file exit=3 → error: not_found: /tmp/.../nope.opj
garbage exit=4     → error: parse: json: [json.exception.parse_error.101] ...
```
9. **Goldens bit-exact, none added/modified:** `harness/run_goldens.sh build-wp0228/opd` → `run_goldens: 16 fixtures, 0 failures` (+ negative control PASS). No file under `tests/fixtures/` touched (git diff confirms).
10. **CLI error contracts:** `harness/test_cli_errors.sh build-wp0228/opd` → `test_cli_errors: 0 failures`.
11. **Glue-script e2e:** with `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv` and a temporary `build/opd` symlink (removed afterwards): `harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
12. **Sanitizers (`-DDZ_SANITIZE=ON`) of op_tests + op_patterns_tests:** build exit 0; both binaries `ALL PASS (0 failures)`. Note: the sanitizer build is at **Debug**, matching CI (`ci.yml` configures Debug for the sanitizers job) — a Release+ASAN build trips the pre-existing GCC 13 `-Werror=array-bounds` false positive in untouched `src/engine/render.cpp:786` (`out.arrangement = {clip};`, WP-0192 code), already documented by WP-0193b/0193c/WP-0201/WP-0209 handoffs. My `patterns.cpp.o` compiled clean in both configurations; no engine change made or needed.

### Metrics
41 CHECKs (target ≥ 25); +31 lines to shared files, 3 new files (~600 lines incl. tests); goldens 16/16 bit-exact; no golden added/modified; no model field / operation / render change.

### Follow-ups / notes
- `PROPOSE:` (out of scope, not touched): the Release+`-DDZ_SANITIZE=ON` combination fails on the untouched `render.cpp:786` array-bounds false positive (see 12). Whoever owns the renderer could silence it at that line or pin the sanitizers job to Debug in docs — CI already builds Debug so nothing is broken today.
- `wpdraft6/` was already untracked in the checkout before this WP; left untouched and unstaged, as in prior handoffs.
- No spec edit needed per the task's Out-of-scope section (`docs/spec/` unchanged); the task file remains the contract.

### Git
Branch `wp-0228-opd-patterns-pattern-listing` from the integration base HEAD (c49b7d4). Staged explicit paths only: the 5 files above + this task file. Not pushed, not merged.
