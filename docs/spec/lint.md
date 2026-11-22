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
| 12 | warning | `fades_exceed_clip_length` | `arrangement[k]` | `fade_in_beats + fade_out_beats exceed the clip length` | `fade_in_beats + fade_out_beats > double(repeats) * extent + 1e-9` (§WP-0203) |
| 13 | info | `choke_group_single_track` | `tracks[i]` | `choke group has only one track` | `choke_group != 0` and exactly one track shares that value (§WP-0203) |
| 14 | info | `swing_without_odd_sixteenths` | `patterns[j]` | `swing is set but no note starts on an odd 16th` | `swing != 0.0` and no note passes the WP-0166 odd-16th test (§WP-0203) |

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

## Additive rules for recently shipped features (WP-0203)

Rules 12–14 cover the features that landed after WP-0170 — clip fades (WP-0178), sampler choke groups
(WP-0181), pattern swing (WP-0166). They are read-only like every other rule, and rules 1–11 keep their
conditions, severities, paths, messages **and relative order**: the three new loops run **after** rule
11, each emitting in ascending index order. The CLI does not change — all three are warning/info, so a
project that only trips them still exits 0.

**Rule 12 `fades_exceed_clip_length` (warning).** Per clip `k`, ascending: `extent =
op::pattern_extent(patterns[clip.pattern])` (the model helper rule 6 uses; an out-of-range
`clip.pattern`, impossible from a parsed file, counts as extent `0`) and `span = double(clip.repeats) *
extent` — exactly the span the renderer fades across (WP-0178); flag iff `clip.fade_in_beats +
clip.fade_out_beats > span + 1e-9`. `sum == span` exactly is **not** flagged, only a genuine overshoot
(`1e-9` beats is the same absolute guard rule 6 uses). A pattern with no notes and no `length_beats` has
`span == 0`, so any positive fade sum is flagged. The clip trim keys `offset_beats` / `length_beats`
(WP-0193a) are **not** consulted — the renderer ignores them until WP-0193b and lint must agree with
what renders: do not shorten `span` by a clip `length_beats`, and never read `pattern.length_beats`
instead of `pattern_extent`. Rule 12 is independent of rules 5/6, so past-the-end clips are checked too.

**Rule 13 `choke_group_single_track` (info).** Per track `i`, ascending: let `g = tracks[i].choke_group`
and skip when `g == 0` (no group); else count the tracks whose `choke_group` equals `g` and flag when
that count is exactly `1`. A lone member can neither be cut nor cut anything (WP-0181 cuts only *other*
tracks' voices), so such a group is dead configuration. Only `choke_group` values are compared — voice
kind, mute/solo and files are irrelevant.

**Rule 14 `swing_without_odd_sixteenths` (info).** Per pattern `j`, ascending: flag when
`patterns[j].swing != 0.0` (`-0.0` equals `0.0`, so a saved `-0.0` is off) and **no** note of the
pattern is one swing would move, i.e. no note satisfies the shipped WP-0166 test on its own
pattern-local `t`: `q = note.t_beats * 4.0` (exact, power-of-two scale), swung iff `std::floor(q) == q
&& std::fmod(q, 2.0) == 1.0` (so t = 0.25, 0.75, 1.25 … qualify; 0.3 does not). Write that predicate
inline with `std::floor`/`std::fmod` from `<cmath>`, as the file-local `is_odd_sixteenth` in
`src/engine/render.cpp` does — do not call, move or export it, and add no new `dsp.hpp` wrapper. A
pattern with swing set and no notes at all **is** flagged. The rule is arrangement-independent: clip
references, `start_bar` and `offset_beats` do not matter.
