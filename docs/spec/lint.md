# `opd lint` — read-only project sanity checks ("why is my beat silent?") as JSON (spec)

Status: normative (WP-0170). A beat that renders silent is almost always a project mistake: a track with no
notes, a pattern no clip references, a clip placed past the end, a sampler pointing at a missing file, a track
silenced by another track's solo. The loader accepts all of these (they are valid projects). This spec defines
the NEW read-only engine module `op::lint` and the `opd lint` command that reports them deterministically, for
the human and for the AI copilot. It never modifies a project.

## Files
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
  `op::Project` through `engine/model.hpp` as it is today (extents were computed locally at the base; after WP-0162
  the integrated tree uses `op::pattern_extent`).

## API (exactly this, namespace `op::lint`)
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
`op::pattern_extent(pattern)`: its explicit `length_beats` when present (WP-0162), else
`max(t_beats + dur_beats)` over its notes (0 if none). Rule 6 uses this extent; rule 7 (empty arrangement,
legacy playback) is unaffected because `length_beats` has no effect without an arrangement.

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

## JSON (`to_json`)
One compact line, no spaces, key order fixed, terminated by `\n`:
`{"version":1,"errors":E,"warnings":W,"infos":I,"issues":[{"severity":"..","code":"..","path":"..","message":".."},...]}`
with E/W/I the counts per severity; empty list prints `"issues":[]`.

## CLI
`opd lint <proj.opj> [--json]` - loads the project (`op::load_project`), runs `lint_project` with `base_dir` =
the project file's parent directory (same as `opd render`), prints `to_json(...)` to stdout. `--json` is
accepted and ignored. Exit codes: 0 when there is no error-severity issue (warnings/infos alone -> 0); 1 when at
least one error-severity issue exists (JSON is still printed); 2 usage (no file, extra or unknown argument);
3 project file not found; 4 project does not parse (use the existing `fail(...)` helper). In cases 2-4 nothing
is written to stdout.
