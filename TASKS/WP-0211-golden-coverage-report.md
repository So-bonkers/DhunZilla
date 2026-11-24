---
id: WP-0211
title: `opd golden-coverage` — which model features each golden .opj exercises and which none covers (canonical JSON report; read-only, not a CI gate)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, cli:main, spec:golden-coverage, new:src/engine/feature_coverage.hpp, new:src/engine/feature_coverage.cpp, new:tests/test_feature_coverage.cpp]
---

## Context - why this exists

Phase 1 exit hygiene. The golden harness proves every committed `.opj` renders bit-exact, but nobody can
answer the other half of the question: **which model features do those goldens actually exercise?** If no
golden has a transport loop region, an attachment, a note velocity, a pattern id, an explicit pattern length
or a non-48 kHz sample rate, a later refactor of that feature can silently break a real song while CI stays
green. This package adds a read-only report, `opd golden-coverage`, that scans golden projects and prints,
in canonical JSON, which features each one exercises plus the features NO golden covers — so drafters know
where a golden is missing. The feature list is a plain table in the tool; it reads files only, adds no model
field, no operation and no render path, and is **not** wired into any gate script.

Existing code this package reuses (read it first, do NOT reimplement or modify it): `op::Project` /
`op::Track` / `op::Voice` / `op::Pattern` / `op::Note` / `op::Clip` / `op::TempoPoint` / `op::LoopRegion` /
`op::Attachment` / `op::TimeSignature` / `op::load_project` in `src/engine/model.hpp`; the canonical-JSON
style of `op::tracks_json` in `src/engine/tracks.cpp` (nlohmann `dump(2) + "\n"`; nlohmann is already on
`opengine`'s include path via `third_party`); the CLI shape of `cmd_clips` / `cmd_tracks` and the shared
`fail(code, detail)` helper in `src/cli/main.cpp`.

## Spec - normative (this task file is the contract)

`docs/spec/golden-coverage.md` carries the same contract and is **already in the tree** (written by the
runner) — read it, but do not edit it or any other file under `docs/`. If it ever disagrees with this task
file, this task file wins.

### Files (exactly these 6; stage no others)
- `src/engine/feature_coverage.hpp` (new) - declares `op::NamedProject` and `op::feature_coverage_json` with a
  doc comment.
- `src/engine/feature_coverage.cpp` (new) - the feature table and the report builder.
- `tests/test_feature_coverage.cpp` (new) - own `main`, own `CHECK(cond, name)` printing `PASS <name>` /
  `FAIL <name>` (copy the style of `tests/test_tracks.cpp`), returns the failure count; projects it writes go
  in one per-process scratch dir (`std::filesystem::temp_directory_path()` + a name containing the pid), never
  fixed `/tmp/op_*`; do NOT edit `tests/test_main.cpp`.
- `CMakeLists.txt` - purely additive, exactly the three blocks under "CMake" below.
- `src/cli/main.cpp` - purely additive: ONE include line, ONE usage line, ONE `static int cmd_golden_coverage`
  helper placed immediately before `int main(`, ONE dispatch line. `opd` is a single-file target: never add a
  new CLI source file.
- `docs/spec/golden-coverage.md` - already present; leave it byte-identical.

### Engine interface (exact)

```cpp
namespace op {

struct NamedProject {
    std::string name;      // label printed by the report (the CLI passes the .opj file stem)
    Project project;       // already loaded by op::load_project
};

std::string feature_coverage_json(const std::vector<NamedProject>& goldens);

}  // namespace op
```

- Pure function: no I/O, no clock, no randomness; it never modifies a project; two calls with equal input
  return byte-identical text.
- Return value: a nlohmann object dumped with `dump(2)` (two-space indent, object keys alphabetically sorted)
  plus exactly ONE trailing `'\n'`. So the text starts `{\n  "features": [\n` and ends with `}\n`.
- Top-level keys, always present, exactly these four: `"features"`, `"goldens"`, `"uncovered"`, `"version"`.
  `version` is the JSON integer `1`.
- `"features"`: one entry per row of the feature table below, **in table order** (the table is key-ascending).
  Each entry has exactly three keys: `"covered_by"` (array of golden names that exercise it, ascending, empty
  when none does), `"key"`, `"label"`.
- `"goldens"`: one entry per input golden, sorted by `"name"` ascending (regardless of input order). Each
  entry has exactly two keys: `"features"` (the table keys that golden exercises, in table order) and `"name"`.
- `"uncovered"`: the table keys whose `covered_by` is empty, in table order.
- Empty input is valid and NOT an error at engine level: `"goldens"` is `[]` and `"uncovered"` lists every
  table key.
- Every predicate reads the **loaded** `op::Project` — never the raw file text — so loader normalizations are
  what gets reported (e.g. a project whose JSON says `"time_signature": {"num": 3, "den": 4}` loads with
  `beats_per_bar == 3` and NO `time_signature` value: it marks `beats_per_bar_not_4` and NOT `time_signature`).

Shape of the document (`<...>` marks repetition, not literal text):

```json
{
  "features": [
    {
      "covered_by": [
        "clip_trim",
        "mixer"
      ],
      "key": "mute",
      "label": "track mute"
    },
    <one entry per table row, in table order>
  ],
  "goldens": [
    {
      "features": [
        "arrangement",
        "clip_offset_beats"
      ],
      "name": "clip_trim"
    },
    <one entry per golden, name-ascending>
  ],
  "uncovered": [
    "attachments",
    <table keys with no coverage, in table order>
  ],
  "version": 1
}
```

### Feature table (the whole point; copy it exactly — key, label and predicate)

Keys are ASCII-ascending and that IS the table order used everywhere in the report. The `label` is the exact
string to print. A row is true for a project when SOME item of the named kind satisfies the condition
(project-level rows are checked on the project itself).

| key | label | true when |
| --- | --- | --- |
| `arrangement` | explicit arrangement clips | `arrangement` is not empty |
| `attachments` | attachment references | `attachments` is not empty |
| `beats_per_bar_not_4` | beats_per_bar != 4 | `beats_per_bar != 4` |
| `choke_group` | sampler choke group | any track has `choke_group != 0` |
| `clip_fade_in` | clip fade-in | any clip has `fade_in_beats > 0` |
| `clip_fade_out` | clip fade-out | any clip has `fade_out_beats > 0` |
| `clip_gain_db` | clip gain dB | any clip has `gain_db != 0` |
| `clip_length_beats` | clip trim length | any clip has `length_beats` present |
| `clip_offset_beats` | clip trim offset | any clip has `offset_beats != 0` |
| `clip_repeats` | clip repeats > 1 | any clip has `repeats != 1` |
| `loop_region` | transport loop region | `loop` is present |
| `master_gain_db` | master fader | `master_gain_db != 0` |
| `metronome_accent` | metronome accent_every != 4 | any voice has `accent_every != 4` |
| `mute` | track mute | any track has `mute` true |
| `note_pitch` | note MIDI pitch | any note has `midi_note >= 0` |
| `note_velocity` | note velocity != 1.0 | any note has `velocity != 1.0` |
| `pan` | track pan != 0 | any track has `pan != 0` |
| `pattern_id` | named pattern | any pattern has a non-empty `id` |
| `pattern_length_beats` | explicit pattern length | any pattern has `length_beats` present |
| `sample_rate_not_48k` | sample_rate != 48000 | `sample_rate != 48000` |
| `sampler_repitch` | sampler root_note (repitch) | any voice has `root_note >= 0` |
| `sampler_slice` | sampler slice end_s | any voice has `has_end_s` true |
| `solo` | track solo | any track has `solo` true |
| `swing` | pattern swing != 0 | any pattern has `swing != 0` |
| `synth_v1` | synth voice v1 | any voice has `waveform != Waveform::Legacy` or `has_adsr` true or `cutoff_hz > 0` |
| `tempo_map` | tempo map | `tempo_map` is not empty |
| `time_signature` | time signature (den != 4) | `time_signature` is present |
| `track_gain_db` | track fader | any track has `gain_db != 0` |
| `voice_kind_metronome` | metronome voice | any voice `kind == VoiceKind::Metronome` |
| `voice_kind_sampler` | sampler voice | any voice `kind == VoiceKind::Sampler` |
| `voice_kind_sine` | sine voice | any voice `kind == VoiceKind::Sine` |
| `voice_kind_synth` | synth voice | any voice `kind == VoiceKind::Synth` |

Nothing else is a feature. Render-only things (asset sample-rate conversion, `--limit-dbfs`, normalize flags)
are NOT model features and must not appear in the table. Do not add, rename, drop or re-order rows.

### CLI (exact)

```
opd golden-coverage <dir|file.opj> [<dir|file.opj> ...]
```

- Each argument is a `.opj` file, or a directory whose `*.opj` files are scanned: names ending in the suffix
  `.opj` (case-sensitive), regular files only, NOT recursive, sorted by file name ascending. Arguments may be
  mixed and repeated; a file reached twice contributes two entries with the same name (no de-duplication).
  There are NO options: any argument starting with `-` is a usage error.
- Each scanned file's report `name` is the file stem (file name with the `.opj` suffix removed), so the report
  never depends on the current working directory.
- The report goes to stdout with `std::fwrite` (exactly the bytes `feature_coverage_json` returned). Nothing
  goes to stdout on an error. Exit codes, using the existing `fail()` helper:
  - `0` success — **including when features are uncovered**; this is a report, not a gate.
  - `2` usage: no arguments → `error: usage: golden-coverage needs at least one <dir or .opj>`; an option →
    `error: usage: unknown arg '<arg>'`; a directory that contains no `.opj` file →
    `error: usage: no .opj files in <the argument exactly as given>`.
  - `3` not-found: a path that does not exist → `error: not_found: <the path exactly as given>`; every argument
    is resolved before ANY project is loaded.
  - `4` parse: the first file that `op::load_project` rejects → `error: parse: <the loader's own message>`
    (never reword or re-derive it).
- Read-only: no file is written, no project is re-saved, no directory is created; two runs on the same tree are
  byte-identical.

### CMake (purely additive, exact lines and anchors)

Immediately after `target_sources(opengine PRIVATE src/engine/tracks.cpp)  # WP-0227`:

```cmake
target_sources(opengine PRIVATE src/engine/feature_coverage.cpp)  # WP-0211
```

Immediately after `add_test(NAME tracks COMMAND op_tracks_tests)`:

```cmake
# WP-0211: opd golden-coverage — which features the goldens exercise. Runs the opd binary, so it needs OPD_BIN.
add_executable(op_feature_coverage_tests tests/test_feature_coverage.cpp)
target_link_libraries(op_feature_coverage_tests PRIVATE opengine)
target_compile_definitions(op_feature_coverage_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
add_dependencies(op_feature_coverage_tests opd)
add_test(NAME feature_coverage COMMAND op_feature_coverage_tests)
```

Immediately after `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227`:

```cmake
list(APPEND DZ_OUR_TARGETS op_feature_coverage_tests)  # WP-0211
```

### `src/cli/main.cpp` (purely additive, exact lines and anchors)

- After `#include "engine/dsp.hpp"`, add: `#include "engine/feature_coverage.hpp"`
- After the `opd tracks` usage line, add:
  `"  opd golden-coverage <dir|file.opj>...   (which model features each golden .opj exercises, as canonical JSON)\n"`
- Add `static int cmd_golden_coverage(int argc, char** argv)` immediately before `int main(`.
- After `if (cmd == "tracks") return cmd_tracks(argc - 1, argv + 1);`, add:
  `if (cmd == "golden-coverage") return cmd_golden_coverage(argc - 1, argv + 1);`

### Determinism / repo rules (ADR-0004)

No wall clock, no unseeded randomness, libm only via `dsp.hpp`, no fast-math; passes `-Werror` and
`-DDZ_SANITIZE=ON`. Tests use a unique temp dir (`std::filesystem::temp_directory_path()` + a name with the
pid), never fixed `/tmp/op_*`. The report must not depend on the locale, the clock or the environment.

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`) and
      `opd golden-coverage tests/fixtures/goldens` exits 0 and leaves exactly these six features uncovered:
      `attachments`, `loop_region`, `note_velocity`, `pattern_id`, `pattern_length_beats`,
      `sample_rate_not_48k` (16 goldens: `arranged`, `clip_fades`, `clip_trim`, `demo`, `imported`, `mixer`,
      `panorama`, `pitched`, `resampled`, `sampler_choke`, `sampler_repitch`, `sampler_trim`, `synth_v1`,
      `tempo`, `time_sig_3_4`, `time_sig_6_8`).
- [x] `opd golden-coverage tests/fixtures/goldens` attributes exactly these single-golden features:
      `choke_group` -> only `sampler_choke`; `sampler_repitch` -> only `sampler_repitch`; `sampler_slice` ->
      only `sampler_trim`; `swing` -> only `clip_trim`; `tempo_map` -> only `tempo`; `master_gain_db` -> only
      `mixer`; `mute` -> only `mixer`; `solo` -> only `mixer`; `clip_length_beats` -> only `clip_trim`;
      `synth_v1` -> only `synth_v1`; `time_signature` -> only `time_sig_6_8`; and `clip_fade_in` ->
      `clip_fades` while `clip_fade_out` -> `clip_fades`, `clip_trim`.
- [x] `tests/test_feature_coverage.cpp` with at least 30 CHECKs covering: the exact 32 table keys in table
      order and non-empty labels; the four top-level keys and `version` 1; empty input (no goldens, all keys
      uncovered); `mute` vs `solo`, `clip_fade_in` vs `clip_fade_out`, `clip_offset_beats` vs
      `clip_length_beats`, `sampler_slice` vs `sampler_repitch`, `note_pitch` vs `note_velocity` staying
      separate; `beats_per_bar = 3` marking `beats_per_bar_not_4` but NOT `time_signature` while 6/8 marks
      both; a legacy synth NOT marking `synth_v1`; `tempo_map`/`loop_region`/`attachments`/`master_gain_db`,
      `pattern_id`/`pattern_length_beats`/`swing`, `sample_rate_not_48k`/`track_gain_db`/`pan`,
      `metronome_accent`/`clip_repeats`/`clip_gain_db`; goldens and `covered_by` sorted by name whatever the
      input order; `uncovered` shrinking; byte-identical repeat calls; and the CLI contract (exit 0 with gaps,
      exit 2 with no arguments and for a directory with no `.opj`, exit 3 for a missing path, exit 4 with empty
      stdout for an unparseable project, unknown option exit 2, two runs byte-identical, project files
      unchanged after a run).
- [x] `opd golden-coverage` stdout is exactly `dump(2)` + one trailing newline (first bytes `{\n  "features": [\n`).
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_feature_coverage_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, and this WP's own
      sources compiling warning-free under the sanitizer flags
      (`g++ -std=c++20 -Wall -Wextra -Werror -ffp-contract=off -fsanitize=address,undefined -Isrc -Ithird_party -c`
      on `src/engine/feature_coverage.cpp` and `src/cli/main.cpp`). The whole-tree `-DDZ_SANITIZE=ON` build is
      ALREADY broken at this base inside `src/engine/render.cpp` (`pattern_mode_project`, `-Werror=array-bounds`)
      — that is pre-existing and out of scope: do not fix it, do not weaken the flags, do not add sanitizer
      exceptions.

## Out of scope - do NOT touch
- `ROADMAP.md`, golden fixtures (`tests/fixtures/`), `harness/` (do NOT wire this command into
  `run_goldens.sh`, `test_cli_errors.sh` or any CI gate), `.github/`, `ai-sidecar`, the JUCE app tree
  `src/app`, the document layer `src/doc`,
  `src/engine/model.hpp`, `src/engine/model.cpp`, `src/engine/model_json.hpp`, `src/engine/render.cpp`,
  `src/engine/ops.cpp`, `tests/test_main.cpp`, and every `docs/` file (the spec file is already written).
- No new model field, no new operation, no change to any render path or to project serialization.
- No feature-table row may be added, dropped, renamed or re-ordered; no extra JSON keys.

## Git
- Branch `wp-0211-golden-coverage-report` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0211: opd golden-coverage — golden feature-coverage report as canonical JSON`. Stage explicit paths only (never `git add -A`):
  `src/engine/feature_coverage.hpp src/engine/feature_coverage.cpp tests/test_feature_coverage.cpp src/cli/main.cpp CMakeLists.txt docs/spec/golden-coverage.md TASKS/WP-0211-golden-coverage-report.md`.
  Include `TASKS/WP-0211-golden-coverage-report.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the six "Files" entries, nothing else)
- `src/engine/feature_coverage.hpp` (new) — `op::NamedProject` + `op::feature_coverage_json` with a doc
  comment and usage example; pure-function contract stated.
- `src/engine/feature_coverage.cpp` (new) — 32 boring predicate functions (one per table row), the
  `kFeatures` table (key, label, predicate; keys ASCII-ascending = table order), and the report builder:
  goldens ordered name-ascending via `std::stable_sort`, predicates evaluated once per golden into a hit
  matrix, `covered_by` collected in that same order (so ascending; duplicate names repeat, no de-dup),
  nlohmann `dump(2) + "\n"`. No I/O, no clock, no RNG, project never modified.
- `tests/test_feature_coverage.cpp` (new) — own `main`, own `CHECK` copied from `tests/test_tracks.cpp`,
  **73 CHECKs** (≥ 30 required), per-process scratch dir `temp_directory_path()/op_feature_coverage_wp0211_<pid>`
  (never fixed `/tmp/op_*`), CLI contract via `OPD_BIN`. `tests/test_main.cpp` untouched.
- `CMakeLists.txt` — purely additive (+9 lines, 0 deletions), exactly the three specified blocks at the
  three specified anchors (opengine source; `op_feature_coverage_tests` with `OPD_BIN` + `add_dependencies`
  + `add_test`; `DZ_OUR_TARGETS` entry).
- `src/cli/main.cpp` — purely additive (+61 lines, 0 deletions): one include (after `engine/dsp.hpp`), one
  usage line (after the `opd tracks` line), `static int cmd_golden_coverage` immediately before `int main(`,
  one dispatch line after the `tracks` dispatch. No new CLI source file.
- `docs/spec/golden-coverage.md` — not edited; committed byte-identical as installed by the runner.
- Out-of-scope list respected: no fixture/harness/model/render/ops/docs edits, no new model field, op or
  render path, no feature row added/renamed/re-ordered, no extra JSON keys, nothing wired into any gate.

### Implementation notes
- `cmd_golden_coverage` follows the repo's `cmd_*` argv convention (`argv[0]` is the subcommand; argument
  loop starts at `i = 1`), like every other command in the file.
- Two passes, in the order the spec demands: resolve (existence, then directory scan: non-recursive,
  regular files whose name suffix is exactly `.opj`, `std::stable_sort` by file name) for EVERY argument
  before ANY `load_project` — so a missing path in a mixed batch still exits 3 without loading anything.
  Load pass in scan order; first loader rejection exits 4 with the loader's message verbatim. Report names
  are file names minus the `.opj` suffix (computed by suffix-strip, not `fs::path::stem`, so non-`.opj`
  names keep theirs); the report is CWD-independent. Report written only via one `std::fwrite` at the end;
  nothing reaches stdout on any error.
- An unscannable directory (iterator error — the spec table doesn't cover it) exits via
  `fail("read", …)` → 5; boring and explicit, never tested for.

### How verified (all on this machine: aarch64 DGX Spark, g++ 13; build via the dz-build skill)
Standard gates:
```
$ bash agent-build-kit/loop/tools/dz_build.sh openpattern --out openpattern/build-wp0211   # -j4, -Werror
… /home/shubhankar_dgx/qwen-scratch/openpattern/build-wp0211        (exit 0; grep -c "warning" → 0 in the log)
$ ./build-wp0211/op_feature_coverage_tests; tail -1 → ALL PASS (0 failures)     # exit 0, 73 PASS lines, 0 FAIL
$ ./build-wp0211/op_tests;                tail -1 → ALL PASS (0 failures)       # exit 0
$ bash harness/run_goldens.sh build-wp0211/opd | tail -1
run_goldens: 16 fixtures, 0 failures                                            # 17 PASS lines, exit 0 → goldens bit-identical
$ bash harness/test_cli_errors.sh build-wp0211/opd | tail -1
test_cli_errors: 0 failures                                                     # 84 PASS lines, exit 0
$ ln -s …/build-wp0211/opd build/opd; UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv \
      bash harness/test_glue_scripts.sh | tail -1
test_glue_scripts: 0 failures                                                   # 21 PASS lines, exit 0; symlink removed after
$ ctest --test-dir build-wp0211
100% tests passed, 0 tests failed out of 19                                     # includes new "feature_coverage" (Test #19)
```
Golden acceptance (exit 0 despite gaps; 16 goldens; exactly the six uncovered; every single-golden
attribution asserted):
```
$ ./build-wp0211/opd golden-coverage tests/fixtures/goldens > gc.json; echo $?
0
goldens (16): ['arranged', 'clip_fades', 'clip_trim', 'demo', 'imported', 'mixer', 'panorama', 'pitched',
 'resampled', 'sampler_choke', 'sampler_repitch', 'sampler_trim', 'synth_v1', 'tempo', 'time_sig_3_4', 'time_sig_6_8']
uncovered (6): ['attachments', 'loop_region', 'note_velocity', 'pattern_id', 'pattern_length_beats', 'sample_rate_not_48k']
ALL GOLDEN ATTRIBUTION CHECKS PASS   # python asserts: choke_group→[sampler_choke]; sampler_repitch→[sampler_repitch];
                                     # sampler_slice→[sampler_trim]; swing→[clip_trim]; tempo_map→[tempo]; master_gain_db,
                                     # mute, solo→[mixer]; clip_length_beats→[clip_trim]; synth_v1→[synth_v1];
                                     # time_signature→[time_sig_6_8]; clip_fade_in→[clip_fades]; clip_fade_out→[clip_fades, clip_trim]
```
Canonical shape + determinism + read-only (also unit-tested, incl. project-file bytes unchanged and zero new
files after a run):
```
first bytes b'{\n  "features": [\n' → True;   last bytes b'}\n' (single trailing '\n') → True
$ diff <(./build-wp0211/opd golden-coverage tests/fixtures/goldens) <(./build-wp0211/opd golden-coverage tests/fixtures/goldens)   # no diff
$ (cd /tmp && build-wp0211/opd golden-coverage <abs path>/tests/fixtures/goldens)   # byte-identical to the repo-root run (name = stem)
```
Sanitizer requirement (this WP's own sources, exactly the specified flags):
```
$ g++ -std=c++20 -Wall -Wextra -Werror -ffp-contract=off -fsanitize=address,undefined -Isrc -Ithird_party \
      -c src/engine/feature_coverage.cpp -o …/feature_coverage.o    # OK, no output
$ … -c src/cli/main.cpp -o …/main.o                                 # OK, no output
```
Whole-tree `-DDZ_SANITIZE=ON` (dz_build.sh --sanitize, build-san-wp0211): fails with exactly one error —
the pre-existing `render.cpp` one (`__builtin_memmove … out of the bounds … const op::Clip [1]
-Werror=array-bounds=` in `pattern_mode_project`), byte-for-byte the failure already present at this base
(`build-san-wp0227/dz_build.log` from WP-0227 shows the identical error). `src/engine/feature_coverage.cpp`
appears in that log as `Building CXX object …` — it compiled clean under the tree's sanitizer flags before
the render.cpp failure. Left unfixed, flags not weakened, no exceptions added — per the task.

### Metrics
- Feature table: 32 rows, key-ascending; 16 committed goldens; 6 uncovered = the "add a golden here" list.
- New LOC: hpp 37, cpp 302, tests 748; CMake +9; main.cpp +61. 73 unit/CLI CHECKs in the new binary.
- `op_feature_coverage_tests` ctest runtime 0.03 s.

### Follow-ups / notes
- The six uncovered features (`attachments`, `loop_region`, `note_velocity`, `pattern_id`,
  `pattern_length_beats`, `sample_rate_not_48k`) are exactly the gap list this report exists to produce —
  candidates for future golden WPs (not done here; fixtures are out of scope).
- `wpdraft6/` (untracked, predates this task — a runner artifact) was left untouched and NOT staged; the
  Git section mandates explicit-path staging only. All other work is committed on
  `wp-0211-golden-coverage-report`; nothing pushed or merged.
- No `PROPOSE:` notes — nothing outside scope needed to change.
