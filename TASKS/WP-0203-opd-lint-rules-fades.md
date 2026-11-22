---
id: WP-0203
title: "`opd lint` rules 12-14 — fades longer than the clip span, lone choke groups, swing with no odd 16ths (read-only)"
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [cmake, spec:lint, src/engine/lint.cpp, new:tests/test_lint_rules.cpp]
---

## Context — why this exists

`opd lint` (WP-0170, `docs/spec/lint.md`) answers "why is my beat silent?" from the project alone. Its
rules were written before three features shipped, so they cannot see mistakes those features make
possible: a clip whose two fade ramps together are longer than the clip itself (WP-0178) never reaches
full level; a `choke_group` on a single track (WP-0181) can never cut anything; and `swing` on a pattern
whose notes all sit on even 16ths (WP-0166) does nothing at all. This package adds **exactly three new
read-only rules** to the existing module, each with a stable code and an exact message string documented
in `docs/spec/lint.md`. Nothing else changes: rules 1–11 keep their conditions, severities, paths,
messages **and relative order**, and the CLI is untouched.

Read (do not change) these for context: `src/engine/lint.hpp` (the `Issue` struct and the two functions
you extend), `docs/spec/lint.md` (rules 1–11 and the JSON shape), `src/engine/model.hpp`
(`Clip::fade_in_beats/fade_out_beats`, `Track::choke_group`, `Pattern::swing`, `op::pattern_extent`).

## Spec — normative

Three new rules, numbered **12, 13, 14**, appended to the rule table in `docs/spec/lint.md`. They run
**after** rule 11 and each emits its issues in ascending index order, exactly like the existing rules:
first all rule-12 issues over `arrangement` (ascending `k`), then all rule-13 issues over `tracks`
(ascending `i`), then all rule-14 issues over `patterns` (ascending `j`). Severity, path and message are
byte-exact — the copilot and the tests match on them.

| # | severity | code | path | message | condition |
|---|---|---|---|---|---|
| 12 | warning | `fades_exceed_clip_length` | `arrangement[k]` | `fade_in_beats + fade_out_beats exceed the clip length` | `clip.fade_in_beats + clip.fade_out_beats > span + 1e-9` where `span = double(clip.repeats) * extent` and `extent = op::pattern_extent(p.patterns[clip.pattern])` |
| 13 | info | `choke_group_single_track` | `tracks[i]` | `choke group has only one track` | `tracks[i].choke_group != 0` **and** exactly one track of the project has that `choke_group` value |
| 14 | info | `swing_without_odd_sixteenths` | `patterns[j]` | `swing is set but no note starts on an odd 16th` | `patterns[j].swing != 0.0` **and** no note of that pattern satisfies the WP-0166 odd-16th test below |

**Rule 12 — fades vs clip span.** `extent` is the model helper `op::pattern_extent()` (explicit
`Pattern::length_beats`, else `max(note.t + note.dur)`, `0.0` for a pattern with no notes) — the same
helper rule 6 uses; do **not** read `pattern.length_beats` yourself. If `clip.pattern` is out of range
(impossible from a parsed file, possible in a hand-built struct) treat the extent as `0.0`, like rule 6
does. `span` is therefore exactly the span the renderer fades across (WP-0178: one envelope per clip
over all its repeats). Flag only a genuine overshoot: `sum == span` **exactly** is NOT flagged, and
`1e-9` beats is the same absolute guard rule 6 uses for its comparison. Consequences to keep:
- a pattern with no notes and no `length_beats` has `span == 0`, so any positive fade sum IS flagged;
- both fades zero ⇒ never flagged (sum 0 can't exceed a non-negative span);
- the clip trim keys `offset_beats` / `length_beats` (WP-0193a) are **NOT** consulted — the renderer
  ignores them until WP-0193b, and lint must agree with what actually renders;
- rule 12 is independent of rules 5/6: a past-the-end or overrunning clip is checked too (no skipping).

**Rule 13 — lone choke group.** Let `g = tracks[i].choke_group`; skip the track when `g == 0` (no
group). Otherwise count the tracks of the whole project whose `choke_group` equals `g`; flag when that
count is exactly `1`. Rationale: WP-0181 cuts only *other* tracks' voices, so a one-member group is
dead configuration. Compare values only — do not look at voice kinds, mute/solo, or the filesystem.

**Rule 14 — swing with nothing to swing.** Flag when `patterns[j].swing != 0.0` (note `-0.0 == 0.0`, so
a saved `-0.0` means off and is NOT flagged) **and** no note of that pattern is a note swing would move.
Only the note's own **pattern-local** `t` matters (`dur` never does), and a note is moved iff the shipped
WP-0166 test passes:

```cpp
const double q = n.t_beats * 4.0;                                    // exact: power-of-two scale
// swung iff std::floor(q) == q && std::fmod(q, 2.0) == 1.0   → t = 0.25, 0.75, 1.25, ... qualify
```

Write that predicate inline in `lint.cpp` with `std::floor` / `std::fmod` from `<cmath>`, exactly as the
file-local `is_odd_sixteenth` in `src/engine/render.cpp` does. Do **not** call, move, export or duplicate
that function across files, and do **not** add a wrapper to `engine/dsp.hpp` (both calls are exact IEEE
operations). A pattern with swing set and **no notes at all** IS flagged. The rule is independent of the
arrangement: whether any clip references the pattern, and any `start_bar` / `offset_beats`, do not matter.

**Unchanged behaviour.** `lint_project(p, base_dir)` stays a pure function of `p`, `base_dir` and (rule 8
only) the filesystem; it never modifies the project. `to_json` is untouched — the new rules just add
issues, so `"errors"/"warnings"/"infos"` counts follow automatically. All three severities are
warning/info, so a project that only trips the new rules still exits **0** from `opd lint`.

## Implementation guide (3 files + CMake)

1. `src/engine/lint.cpp` — **purely additive**: no existing line may be modified or removed. Add
   `#include <cmath>` next to the other standard includes, and append the three loops after the rule-11
   loop, immediately before `return out;`. Match the file's existing style (one commented loop per rule,
   `out.push_back({"severity", "code", "path", "message"})`, `"arrangement[" + std::to_string(k) + "]"`
   for paths). No new files, no change to `src/engine/lint.hpp`.
2. `docs/spec/lint.md` — **purely additive**: three new rows after the rule-11 row of the table, plus a
   new section `## Additive rules for recently shipped features (WP-0203)` at the end documenting each
   rule's code, severity, path, exact message and condition (the formulas above, verbatim). Do not delete
   or reword the documentation of rules 1–11 — every existing code must still appear in the file. A
   complete replacement of this file is provided to you as `00-WP-0203-opd-lint-rules-fades.spec.lint.md`;
   if the runner has already copied it over, just verify the three codes and messages are there.
3. `tests/test_lint_rules.cpp` (**new file**, this exact path) — its own `main`, local
   `CHECK(cond, name)` macro printing `PASS <name>` / `FAIL <name>` (copy the style of
   `tests/test_lint.cpp`), returns the failure count. **≥ 20 CHECKs** covering everything in the Spec
   section: rule 12 flagged case with exact severity/path/message; `sum == span` exactly not flagged;
   `sum < span` not flagged; `repeats` widening the span (fits / overshoots); explicit
   `Pattern::length_beats` as the extent (not flagged at equality, flagged once it is removed); clip
   trim keys ignored (a clip with `length_beats = 0.25` and fades summing to the untrimmed span is NOT
   flagged); zero fades never flagged; empty pattern (extent 0) with a fade flagged; when two clips of
   one project differ only in fades, exactly one issue whose path names the offending clip; rule 13 lone
   group flagged with exact severity/path/message, a matching pair not flagged, two different lone
   groups both flagged, `choke_group == 0` never flagged; rule 14 swing off (`0.0` and `-0.0`) not
   flagged, swing with only even-step notes flagged with the exact message, a note at `t = 0.75` not
   flagged, `t = 0.3` flagged, no-notes-with-swing flagged; one **exact JSON string** comparison of
   `to_json(lint_project(...))` for a project whose only issues are the three new ones (asserts rule
   order 12 → 13 → 14 and all three messages); and that an existing rule's issue (e.g. `clip_past_end`)
   still comes before a new one on the same clip. If you need a sampler path to resolve, point it at
   `/dev/null` or use a pid-scoped temp dir under `std::filesystem::temp_directory_path()` — never a
   fixed `/tmp/op_*` path.
4. `CMakeLists.txt` — **purely additive**: right after the line `add_test(NAME lint COMMAND op_lint_tests)`
   add exactly
   ```cmake
   add_executable(op_lint_rules_tests tests/test_lint_rules.cpp)
   target_link_libraries(op_lint_rules_tests PRIVATE opengine)
   add_test(NAME lint_rules COMMAND op_lint_rules_tests)
   ```
   and after `list(APPEND DZ_OUR_TARGETS op_clip_trim_tests)  # WP-0193a` add
   `list(APPEND DZ_OUR_TARGETS op_lint_rules_tests)  # WP-0203`.

## Acceptance criteria

- [ ] Everything above implemented; rules 1–11 unchanged in code and output (their diff hunks are purely
      additive — no existing line of `src/engine/lint.cpp` modified or deleted).
- [ ] `opd lint` on a project with one sine track, one pattern (extent 1.0 beat) and one clip with
      `fade_in_beats = fade_out_beats = 1.0` prints exactly one warning
      `{"severity":"warning","code":"fades_exceed_clip_length","path":"arrangement[0]","message":"fade_in_beats + fade_out_beats exceed the clip length"}` and exits 0; with `0.5/0.5` (sum == span) it prints no issues at all.
- [ ] Rounding stays inside the guard: a pattern whose extent is `0.3` (`{"t": 0.0, "dur": 0.3}`) with a
      clip `fade_in_beats = 0.1`, `fade_out_beats = 0.2` prints **no** issues (binary `0.1 + 0.2` exceeds
      `0.3` by ~5.6e-17, well inside `1e-9`). Do not drop or widen the `1e-9`: a clip with
      `fade_in_beats = 0.5001` on a pattern of extent `0.5` (an overshoot of ~1e-4 beats) IS reported.
- [ ] A lone sampler track with `"choke_group": 3` yields exactly one info on `tracks[0]` with message
      `choke group has only one track`; two tracks sharing a group yield none; groups `1` and `2` on two
      tracks yield two.
- [ ] A pattern with `"swing": 0.5` and notes only at `t = 0.0/0.5` yields exactly one info on
      `patterns[j]` with message `swing is set but no note starts on an odd 16th`; adding a note at
      `t = 0.75` removes it; `"swing": 0` or `-0.0` never yields it.
- [ ] Order: in one project the three new issues appear after every rule 1–11 issue, in the order
      12 → 13 → 14 (asserted by an exact JSON comparison in the tests).
- [ ] `tests/test_lint_rules.cpp` with ≥ 20 CHECKs; `build/op_lint_rules_tests` ends `0 failure(s)`;
      `build/op_lint_tests` and `build/op_tests` still end with 0 failures.
- [ ] Standard gates: `-Werror` build (0 warnings, `-j4`), `op_tests`, `op_lint_tests`,
      `op_lint_rules_tests`, `bash harness/run_goldens.sh` (15 fixtures, unchanged bits),
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`, and a sanitizer build
      (`-DDZ_SANITIZE=ON`) of `opd` + `op_lint_rules_tests` run clean.
- [ ] ADR-0004 respected: no clock, no randomness, no new libm wrappers; the only new `<cmath>` calls are
      `std::floor` and `std::fmod`, identical to the shipped swing predicate.

## Out of scope — do NOT touch

- `ROADMAP.md` (the elephant adds the row at ship time), every existing golden fixture under
  `tests/fixtures/goldens/`, `harness/` scripts, `CMakeLists.txt` beyond the additive lines above.
- `src/engine/lint.hpp`, `src/cli/main.cpp`, `src/engine/model.*`, `src/engine/ops.cpp`,
  `src/engine/render.cpp`, `src/engine/dsp.hpp`, `src/doc/`, `src/app/`, `ai-sidecar/`, `docs/adr/`,
  `tests/test_main.cpp`, `tests/test_lint.cpp` and every other test file: **no CLI, model, render or op
  change is needed** — the rules are pure reads of `op::Project`.
- No new rule beyond 12–14 (no fade-vs-tempo-map reasoning, no crossfade detection between clips, no
  "choke group on a non-sampler track" rule — the parser already rejects that), no severity or ordering
  change to rules 1–11, no `--strict`/`--only` CLI flags, no JSON schema change, no fix-up suggestions in
  messages, and no attempt to make lint aware of clip trim (`offset_beats` / `length_beats`).

## Git

- Branch `wp-0203-opd-lint-rules-fades` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0203: opd lint rules 12-14 (fades vs clip span, lone choke group, dead swing) + spec + own test file`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/lint.cpp` — purely additive (+48 lines, 0 modified/deleted): `#include <cmath>` inserted among the
    standard includes, and three commented loops appended after rule 11, immediately before `return out;`:
    rule 12 `fades_exceed_clip_length` (warning, `arrangement[k]`, span = `repeats * op::pattern_extent()`, guard
    `+ 1e-9`, trim keys ignored), rule 13 `choke_group_single_track` (info, `tracks[i]`, lone non-zero group only),
    rule 14 `swing_without_odd_sixteenths` (info, `patterns[j]`, inline `std::floor(q) == q && std::fmod(q, 2.0) == 1.0`
    predicate identical to render.cpp's file-local `is_odd_sixteenth`). No new files under `src/`, no header change.
  - `docs/spec/lint.md` — the runner-installed complete replacement (`00-WP-0203-…spec.lint.md`) was already in the
    working tree at start; verified it contains all three codes, severities, paths and byte-exact messages plus the
    `## Additive rules for recently shipped features (WP-0203)` section, and committed it **unchanged**. Rules 1–11
    documentation untouched (diff is +38 lines only).
  - `tests/test_lint_rules.cpp` — new file, own `main`, `CHECK(cond, name)` macro copied from `tests/test_lint.cpp`,
    exit code = failure count. **32 CHECKs** covering every Spec bullet (see evidence below), including the exact
    JSON string comparison of all three new issues and sampler path via `/dev/null`.
  - `CMakeLists.txt` — purely additive (+4 lines): the exact three-line `op_lint_rules_tests` block after
    `add_test(NAME lint COMMAND op_lint_tests)` and `list(APPEND DZ_OUR_TARGETS op_lint_rules_tests)  # WP-0203`.
- **How verified:** (all commands run on aarch64 DGX Spark, branch `wp-0203-opd-lint-rules-fades` from HEAD a4bee6b)
  - *Acceptance bullet 1 — purely additive:* `git diff --stat` → `CMakeLists.txt | 4 ++++`, `docs/spec/lint.md | 38 ++…`,
    `src/engine/lint.cpp | 48 ++…`; `git diff src/engine/lint.cpp | grep -c "^-[^-]"` → **0** removed lines.
  - *Bullet 2 — CLI fades, exact warning + exit 0 / silence at equality:*
    ```
    == fades_bad   (extent 1.0, fade_in=fade_out=1.0)
    {"version":1,"errors":0,"warnings":1,"infos":0,"issues":[{"severity":"warning","code":"fades_exceed_clip_length","path":"arrangement[0]","message":"fade_in_beats + fade_out_beats exceed the clip length"}]}
    exit=0
    == fades_ok    (0.5/0.5, sum == span)
    {"version":1,"errors":0,"warnings":0,"infos":0,"issues":[]}
    exit=0
    ```
  - *Bullet 3 — rounding:* extent `{"t":0.0,"dur":0.3}` with fades 0.1/0.2 → `{"…warnings":0…"issues":[]}` exit=0;
    extent 0.5 with `fade_in_beats=0.5001` → the one warning above, exit=0 (`round_ok` / `round_bad` probes).
  - *Bullet 4 — choke groups:* lone sampler track `"choke_group": 3` (path `/dev/null`) → exactly one info
    `{"severity":"info","code":"choke_group_single_track","path":"tracks[0]","message":"choke group has only one track"}` exit=0;
    two tracks sharing group 3 → `"infos":0,"issues":[]`; groups 1 and 2 → two infos on `tracks[0]` and `tracks[1]`.
  - *Bullet 5 — swing:* `"swing": 0.5` with notes only at t = 0.0/0.5 → exactly one info
    `{"severity":"info","code":"swing_without_odd_sixteenths","path":"patterns[0]","message":"swing is set but no note starts on an odd 16th"}` exit=0;
    adding a note at t = 0.75 → `"issues":[]`; `"swing": 0` and `-0.0` → `"issues":[]`.
  - *Bullet 6 — order:* asserted by `exact_json_rules_12_13_14_in_order`, an exact `to_json(lint_project(...))` string
    comparison whose only issues are the three new ones in 12 → 13 → 14 order (`PASS`), plus
    `rule12_after_existing_rule_on_same_clip` (`clip_past_end` on `arrangement[0]` precedes the rule-12 issue on the same clip).
  - *Bullet 7 — test binaries:* `build/op_lint_rules_tests` prints 32 `PASS` lines and ends `0 failure(s)` (exit 0);
    `build/op_lint_tests` ends `0 failure(s)`; `build/op_tests` exit 0, last line `ALL PASS (0 failures)`.
  - *Bullet 8 — standard gates:* `-Werror` build (`DZ_WERROR=ON`, `-j4`) rebuilt with **0** lines matching "warning";
    `harness/run_goldens.sh` → `run_goldens: 15 fixtures, 0 failures` (+ negative control);
    `harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`;
    `UV_CACHE_DIR=… bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
    Sanitizer build (`cmake -DDZ_SANITIZE=ON` in `build-san-wp0203`, target `opd op_lint_rules_tests`): tests binary
    ends `0 failure(s)` with 0 stderr bytes and no ASAN/UBSAN report; `build-san-wp0203/opd lint` on the fades / choke /
    swing probes → exit=0, stderr 0 bytes each.
  - *Bullet 9 — ADR-0004:* new `<cmath>` calls in the diff are exactly one `std::floor` and one `std::fmod`
    (`grep "^+" … | uniq -c` → `1 std::floor`, `1 std::fmod`); no clock, no randomness, no dsp.hpp wrapper.
- **Metrics:** 4 files touched (3 code/build + spec) plus this task file; +90 lines in lint.cpp/CMake/spec diff,
  new test file 368 lines / 32 CHECKs; all gates: unit 32/32, goldens 15/15, CLI contracts 0 failures, glue 0 failures,
  sanitizers clean.
- **PROPOSE / BLOCKED:** nothing blocked. `wpdraft6/` was already untracked in the working tree at session start
  (another agent's drafts) — left untouched per scope rules; it is not part of this commit. No changes outside the
  four files this package names.

