---
id: WP-0170
title: `opd lint` - read-only project sanity checks ("why is my beat silent?") as JSON (new engine module)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.2
human_review: none
---

## Context - why this exists
A beat that renders silent is almost always a project mistake: a track with no notes, a pattern no clip
references, a clip placed past the end, a sampler pointing at a missing file, a track silenced by another
track's solo. The loader accepts all of these (they are valid projects). This package adds a NEW read-only
engine module `op::lint` and an `opd lint` command that reports them deterministically, for the human and for
the AI copilot. It never modifies a project.

## Spec - normative
Create the NEW file `docs/spec/lint.md` containing the content of this whole "Spec" section (headings may be
adapted). Do not edit other spec files.

### Files
- `src/engine/lint.hpp`, `src/engine/lint.cpp` (new). JSON string escaping uses `nlohmann::json(str).dump()`
  (nlohmann is already on the include path via `opengine`).
- `CMakeLists.txt`: purely ADDITIVE edits (do not change or delete any existing line):
  1. directly after the line `target_include_directories(opengine PUBLIC src third_party)` add
     `target_sources(opengine PRIVATE src/engine/lint.cpp)  # WP-0170`;
  2. immediately before the comment line starting `# Our targets only` add:
     `add_executable(op_lint_tests tests/test_lint.cpp)`,
     `target_link_libraries(op_lint_tests PRIVATE opengine)`,
     `add_test(NAME lint COMMAND op_lint_tests)`;
  3. directly after the line `set(DZ_OUR_TARGETS opengine dzdoc opd op_tests)` add
     `list(APPEND DZ_OUR_TARGETS op_lint_tests)  # WP-0170`.
- `tests/test_lint.cpp` (new; do NOT edit `tests/test_main.cpp`): own `main`, local `CHECK(cond, name)` macro
  printing `PASS name` / `FAIL name` (copy the style from `tests/test_main.cpp`), returns the failure count.
- `src/cli/main.cpp`: ONE additive block - `#include "engine/lint.hpp"` after the `engine/dsp.hpp` include;
  one usage line immediately BEFORE the `opd steps` usage line; `static int cmd_lint(...)` immediately BEFORE
  the comment of `parse_decimal` (the line starting `// Parse a decimal argument`); one dispatch line
  `if (cmd == "lint") return cmd_lint(argc - 1, argv + 1);` immediately BEFORE the `if (cmd == "steps")` line.
  No existing line may be modified or removed.
- Do NOT edit `src/engine/model.*`, `ops.cpp`, `render.cpp` or any other engine file: lint only READS
  `op::Project` through `engine/model.hpp` as it is today (do not use `pattern_extent` or any helper that does
  not exist at the base; compute extents locally).

### API (exactly this, namespace `op::lint`)
```cpp
#pragma once
#include <string>
#include <vector>
#include "engine/model.hpp"
namespace op::lint {
struct Issue { std::string severity; std::string code; std::string path; std::string message; };
std::vector<Issue> lint_project(const Project& p, const std::string& base_dir);
std::string to_json(const std::vector<Issue>& issues);
}
```
`severity` is `"error"`, `"warning"` or `"info"`. `lint_project` is a pure function of `p`, `base_dir` and (for
rule 8 only) the filesystem. Issues are emitted rule by rule in the order below; within a rule in ascending
index order. Let `bpb = beats_per_bar` and `len_beats = length_bars * bpb`. A pattern's `extent` is
`max(t_beats + dur_beats)` over its notes (0 if none).

| # | severity | code | path | message | condition |
|---|---|---|---|---|---|
| 1 | warning | `track_no_notes` | `tracks[i]` | `track has no notes` | no pattern with `track == i` has a note |
| 2 | warning | `pattern_empty` | `patterns[j]` | `pattern has no notes` | pattern has zero notes |
| 3 | warning | `pattern_unused` | `patterns[j]` | `pattern is not referenced by any clip` | arrangement non-empty and no clip has `pattern == j` |
| 4 | warning | `clip_duplicate` | `arrangement[k]` | `duplicate of arrangement[m]` | an earlier clip has the same `pattern` and exactly equal `start_bar`; `m` = the FIRST such clip; one issue per clip k |
| 5 | warning | `clip_past_end` | `arrangement[k]` | `clip starts at or after length_bars` | `start_bar >= length_bars` |
| 6 | warning | `clip_overruns_length` | `arrangement[k]` | `clip ends after length_bars` | NOT rule 5, pattern extent > 0, and `start_bar*bpb + repeats*extent > len_beats + 1e-9` |
| 7 | warning | `notes_past_end` | `patterns[j]` | `<N> note(s) start at or after length_bars` | arrangement EMPTY and N >= 1 notes have `t_beats >= len_beats` |
| 8 | error | `sampler_file_missing` | `tracks[i].voice.path` | `sample file not found: <voice.path>` | track voice is a sampler and the file does not exist |
| 9 | info | `track_muted` | `tracks[i]` | `track is muted` | `mute == true` |
| 10 | info | `track_silenced_by_solo` | `tracks[i]` | `silenced because another track is soloed` | some track has `solo == true`, and track i has `solo == false` and `mute == false` |
| 11 | warning | `tempo_point_past_end` | `tempo_map[k]` | `tempo change at or after length_bars` | `tempo_map[k].bar >= length_bars` |

Rule 8 file resolution: `p = voice.path`; if `p` is not absolute and `base_dir` is non-empty, `p = base_dir / p`;
the issue is raised when `std::filesystem::exists(p, ec)` is false (use the `error_code` overload, never throw).

### JSON (`to_json`)
One compact line, no spaces, key order fixed, terminated by `\n`:
`{"version":1,"errors":E,"warnings":W,"infos":I,"issues":[{"severity":"..","code":"..","path":"..","message":".."},...]}`
with E/W/I the counts per severity; empty list prints `"issues":[]`.

### CLI
`opd lint <proj.opj> [--json]` - loads the project (`op::load_project`), runs `lint_project` with `base_dir` =
the project file's parent directory (same as `opd render`), prints `to_json(...)` to stdout. `--json` is
accepted and ignored. Exit codes: 0 when there is no error-severity issue (warnings/infos alone -> 0); 1 when at
least one error-severity issue exists (JSON is still printed); 2 usage (no file, extra or unknown argument);
3 project file not found; 4 project does not parse (use the existing `fail(...)` helper). In cases 2-4 nothing
is written to stdout.

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`).
- [x] `tests/test_lint.cpp` with at least 12 CHECKs: a clean project has no issues; `to_json({})` is exactly
      `{"version":1,"errors":0,"warnings":0,"infos":0,"issues":[]}` + `\n`; one CHECK per rule 1-11 asserting
      code, path AND message; rule ordering (rule 1 issue before rule 2 issue); a clip ending exactly at the end
      of the song does NOT trigger rule 6; a muted track is not also reported by rule 10; exact JSON for one
      warning.
- [x] `opd lint` works as specified (JSON, exit codes).
- [x] Standard gates: build (0 warnings, `-j4`), `op_tests`, `op_lint_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of
      `op_tests` and `op_lint_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds rows), `src/app/`, `.github/`, golden fixtures (`tests/fixtures/`),
  `docs/spec/` other than the new `lint.md`, `docs/adr/`, `ai-sidecar/`, `tests/test_main.cpp`,
  `src/engine/model.*`, `src/engine/ops.cpp`, `src/engine/render.cpp`, `src/engine/midi.cpp`, `src/doc/`.
- Auto-fix, any change to loading/validation rules, new rules beyond the eleven, localisation.

## Git
- Branch `wp-0170-opd-lint` from `main`. Commit: `WP-0170: opd lint - read-only project sanity checks as JSON`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent
- **What changed:**
  - NEW `src/engine/lint.hpp` / `src/engine/lint.cpp`: read-only module `op::lint` with exactly the spec'd API
    (`Issue`, `lint_project`, `to_json`). Eleven rules emitted in spec order, ascending index within each rule;
    rule 8 resolves relative sample paths against `base_dir` and uses `std::filesystem::exists(p, ec)` (never
    throws); JSON escaping via `nlohmann::json(str).dump()`. Extents computed locally (no engine helper used).
  - NEW `docs/spec/lint.md`: the full normative Spec section from this package.
  - NEW `tests/test_lint.cpp`: own `main` + local `CHECK` macro (test_main style), **18 CHECKs** — clean project,
    exact `to_json({})`, one per rule 1-11 (code+path+message asserted), rule 6 exact-end boundary, rule 8
    positive case (existing file), rule ordering (rule 1 before rule 2), muted-track-not-rule-10, exact JSON for
    one warning. `tests/test_main.cpp` untouched.
  - `CMakeLists.txt`: purely ADDITIVE — verified by diff: **6 insertions, 0 deletions** (`target_sources ... lint.cpp`,
    the three `op_lint_tests` lines before `# Our targets only`, `list(APPEND DZ_OUR_TARGETS op_lint_tests)`).
  - `src/cli/main.cpp`: purely ADDITIVE — verified by diff: **32 insertions, 0 deletions** (include after
    `engine/dsp.hpp`; usage line before `opd steps`; `cmd_lint` immediately before the `// Parse a decimal ...`
    comment; dispatch line immediately before `if (cmd == "steps")`). No existing line modified.
- **How verified:** (all commands run on this aarch64 rig, branch `wp-0170-opd-lint`)

  1. Build `-j4`, 0 warnings (`-Werror` on all our targets), forced recompile of every touched source:
     ```
     $ touch src/engine/lint.cpp tests/test_lint.cpp src/cli/main.cpp && cmake --build build -j4   # exit 0
     [  5%] Building CXX object CMakeFiles/opengine.dir/src/engine/lint.cpp.o
     [ 61%] Building CXX object CMakeFiles/op_lint_tests.dir/tests/test_lint.cpp.o
     [ 72%] Building CXX object CMakeFiles/opd.dir/src/cli/main.cpp.o
     [100%] Built target opd          # grep -icE "warning|error" over the log → 0
     ```
  2. `./build/op_lint_tests` — **18 PASS, exit 0**:
     ```
     PASS clean_project_no_issues
     PASS to_json_empty_exact
     PASS rule1_track_no_notes
     PASS rule2_pattern_empty
     PASS rule3_pattern_unused
     PASS rule4_clip_duplicate
     PASS rule5_clip_past_end
     PASS rule6_clip_overruns_length
     PASS rule6_exact_end_not_flagged
     PASS rule7_notes_past_end
     PASS rule8_sampler_file_missing
     PASS rule8_existing_file_no_issue
     PASS rule9_track_muted
     PASS rule10_track_silenced_by_solo
     PASS rule10_muted_track_not_also_reported
     PASS rule11_tempo_point_past_end
     PASS rule_order_rule1_before_rule2
     PASS to_json_one_warning_exact
     0 failure(s)
     ```
     Also registered in CTest: `ctest -R '^lint$'` → `Test #2: lint ... Passed`.
  3. `./build/op_tests` — unchanged suite still green: `ALL PASS (0 failures)` (687 PASS lines), exit 0.
  4. Goldens bit-identical: `bash harness/run_goldens.sh` →
     ```
     PASS arranged: deterministic + golden match
     PASS demo: deterministic + golden match
     PASS imported: deterministic + golden match
     PASS mixer: deterministic + golden match
     PASS panorama: deterministic + golden match
     PASS pitched: deterministic + golden match
     PASS resampled: deterministic + golden match
     PASS sampler_trim: deterministic + golden match
     PASS tempo: deterministic + golden match
     PASS negative-control (corrupt golden rejected)
     run_goldens: 9 fixtures, 0 failures
     ```
  5. `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures` (66 PASS), exit 0.
  6. `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh` →
     `test_glue_scripts: 0 failures`, exit 0.
  7. Sanitizers: `cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4 --target op_tests op_lint_tests`
     (exit 0, 0 warnings), then ran both: `./build-san/op_lint_tests` → `0 failure(s)`; `./build-san/op_tests` →
     `ALL PASS (0 failures)`. No ASAN/UBSAN reports.
  8. CLI contract matrix (real runs against `build/opd`; cases 2-4 stdout verified empty):
     ```
     $ opd lint demo.opj            → {"version":1,"errors":0,"warnings":0,"infos":0,"issues":[]}   exit=0
     $ opd lint demo.opj --json     → exit=0 (--json accepted, ignored)
     $ opd lint nokick.opj          → {"version":1,"errors":1,...,"code":"sampler_file_missing","path":"tracks[0].voice.path",
                                     "message":"sample file not found: kits/missing.wav"}]}          exit=1 (JSON printed)
     $ opd lint                     → exit=2 stdout=''   (no file)
     $ opd lint demo.opj extra.opj  → exit=2 stdout=''   (extra argument)
     $ opd lint demo.opj --wat      → exit=2 stdout=''   (unknown argument)
     $ opd lint nope.opj            → exit=3 stdout=''   (not found)
     $ opd lint bad.opj             → exit=4 stdout=''   (does not parse, via fail())
     ```
  - No audio output affected (lint never renders), so no demo WAVs attached.
- **Metrics:** +542 new lines (spec 77, lint.hpp 18, lint.cpp 172, test_lint.cpp 275) + 38 purely-additive lines in
  existing files; 0 deletions; 18/18 new CHECKs pass; all pre-existing gates green.
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE (out of scope, not done): the `docs/spec/project-model.md` CLI
  exit-code table could gain a pointer to `docs/spec/lint.md`; also a future WP could teach the copilot prompt
  templates to run `opd lint` before proposing fixes. No engine/loader changes were needed or made.
