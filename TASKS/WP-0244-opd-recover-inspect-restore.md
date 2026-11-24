---
id: WP-0244
title: `opd recover <project>` — GUI-free crash-recovery report and restore of the WP-0157 autosave file
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.6
human_review: none
touches: [cmake, cli:main, spec:operations, new:tests/test_recover.cpp]
---

## Context - why this exists

WP-0157 gave `op::Document` atomic autosave and crash recovery (`<path>.autosave`, `recovery_state`,
`open_recovered`), but only the app can reach it: after a crash there is no way to find out from a terminal
whether a project has unsaved work in its autosave file, and no way to get that work back out as a normal
project file. This package adds the CLI front end `opd recover <proj.opj> [--list | --restore -o <out.opj>]`:
a four-state report, and a restore that loads the autosave through `open_recovered` and writes a canonical
`.opj` to `-o`. It never touches the project file unless `-o` names that exact path, and it re-decides no
recovery rule: **`src/doc/` and `src/engine/` are read, never modified.**

Existing code this package reuses (read it first, do NOT reimplement or modify it):
`op::Document::recovery_state` / `op::RecoveryState` / `op::Document::autosave_path` /
`op::Document::open_recovered` / `Document::save_as` (`src/doc/document.hpp`, `src/doc/document.cpp` —
`autosave_path(p)` is `p + ".autosave"`; `recovery_state` returns `AutosaveNewer` iff both files load and
their canonical `op::project_bytes` differ); `op::load_project` / `op::Project` (`src/engine/model.hpp`).
The CLI shape to copy is `cmd_diff` (positional project, `-o` parsed in a loop) in `src/cli/main.cpp`.

## Spec - normative (this task file is the contract)

`docs/spec/operations.md` (new section "Crash-recovery inspection and restore — `opd recover` — additive
(WP-0244)") is **already updated by the runner** before you start. Read it first; it is the contract and
wins on any difference. If that section is NOT in `docs/spec/operations.md`, write BLOCKED in the handoff
and stop; do not invent the contract and do not edit `docs/spec/` yourself. The section says exactly what is
below — no more, no less.

### Files (you write 3; the runner supplies the spec section; stage no others)

- `src/cli/main.cpp` - **purely additive** (zero deleted or modified lines): one include line
  `#include "doc/document.hpp"` above `#include "engine/analyze.hpp"`; one usage line (below); a
  `static int cmd_recover(int argc, char** argv)` placed directly before `int main(`; and one dispatch line
  `if (cmd == "recover") return cmd_recover(argc - 1, argv + 1);  // WP-0244` after the `audition` dispatch.
  Do **not** add a new `src/cli/*.cpp` file: `opd` is a single-file target.
- `tests/test_recover.cpp` (new) - own `main`, own `CHECK(cond, name)` printing `PASS <name>` / `FAIL <name>`
  (copy the style of `tests/test_tracks.cpp`, which also runs the `opd` binary via `OPD_BIN`), returns the
  failure count. One unique temp dir: `std::filesystem::temp_directory_path()` + a name containing
  `getpid()` (e.g. `op_recover_tests_wp0244_<pid>`), removed at the end; never fixed `/tmp/op_*` paths. Do
  NOT edit `tests/test_main.cpp`.
- `CMakeLists.txt` - purely additive, exactly these 7 lines in these places (plus one `# WP-0244: ...`
  comment line of your own wording above the test block if you want it):
  - after the existing line `target_link_libraries(opd PRIVATE opengine)`:
    `target_link_libraries(opd PRIVATE dzdoc)  # WP-0244: opd recover uses the op::Document recovery layer`
  - after the existing line `add_test(NAME tracks COMMAND op_tracks_tests)`:
    ```
    add_executable(op_recover_tests tests/test_recover.cpp)
    target_link_libraries(op_recover_tests PRIVATE opengine)
    target_compile_definitions(op_recover_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
    add_dependencies(op_recover_tests opd)
    add_test(NAME recover COMMAND op_recover_tests)
    ```
  - after the existing line `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227`:
    `list(APPEND DZ_OUR_TARGETS op_recover_tests)  # WP-0244`
- `docs/spec/operations.md` - already updated by the runner; do not edit it.
- `TASKS/WP-0244-opd-recover-inspect-restore.md` - this file, with the handoff filled.

### Interface (exact strings — the hidden check compares bytes)

CLI (a new subcommand; usage text is on stderr like every other command):

```
  opd recover <proj.opj> [--list | --restore -o <out.opj>]   (crash-recovery report / restore)
```

`<proj>` is the single positional argument; `--list`, `--restore` and `-o <path>` are the only options. No
mode flag means `--list`. The autosave file of `<proj>` is exactly `op::Document::autosave_path(proj)`
(`<proj>.autosave`). `<proj>.autosave.tmp` is never read, never reported, never removed.

**States, decided in this order** (they must be the *document layer's* answers, not your own comparison):

1. `recoverable` — `op::Document::recovery_state(proj) == op::RecoveryState::AutosaveNewer`.
2. `missing` — not recoverable and `<proj>.autosave` does not exist.
3. `corrupt` — not recoverable, the file exists and `op::load_project` on it fails.
4. `identical` — not recoverable, the file exists and `op::load_project` on it succeeds.

**Report** (`--list`, or no flag): exactly one stdout line, and exit **0 in all four states** — do not make
"nothing to recover" an error in report mode:

```
recover: <proj>: autosave differs from the saved project (recoverable)
recover: <proj>: no autosave file
recover: <proj>: autosave matches the saved project
recover: <proj>: autosave unreadable (corrupt)
```

**Restore** (`--restore -o <out>`): call `op::Document::open_recovered(proj, doc, err)`, then
`doc.save_as(out, err)`. Never call `doc.save()` — `doc` is open at `proj`, so `save()` would overwrite the
project file. Never call `op::save_project` directly, and never copy the file.

- Success: exit 0, stdout exactly `recover: <proj>: restored <proj>.autosave -> <out>`. The bytes written
  to `<out>` equal the bytes of `<proj>.autosave` (both are `op::project_bytes`). `<proj>` and
  `<proj>.autosave` stay byte-identical and the autosave file stays present — unless `-o` names `<proj>`
  itself, which is the only case where `<proj>` is written; then `save_as` also removes `<proj>.autosave`
  (and it removes a stale `<out>.autosave` of any other `-o` path). Do not add or remove any deletion of
  your own: the deletions above are the document layer's rule and must stay observable.
- state `missing` or `identical`: exit 3, stderr exactly `error: not_found: no recoverable autosave for '<proj>'`, and `<out>` is not created.
- state `corrupt`: exit 4, stderr exactly `error: parse: corrupt autosave '<proj>.autosave'`, and `<out>` is not created.

**Other errors** (existing `fail(code, detail)` helper in `src/cli/main.cpp` maps the code to the exit
number: usage 2, not_found 3, parse 4, anything else 5):

- exit 2 (call `usage()` first, then `fail`): no `<proj>` argument — detail
  `recover needs <proj.opj> [--list | --restore -o <out.opj>]`; unknown argument — detail
  `unknown arg '<arg>'`; `--list` and `--restore` both given — detail `--list and --restore are mutually
  exclusive`; `--restore` without `-o` — detail `recover --restore needs -o <out.opj>`; `-o` without
  `--restore` — detail `-o needs --restore`. In every usage case nothing is written.
- exit 3 when `<proj>` does not exist: `fail("not_found", proj)` (checked before anything is loaded).
- exit 4 when `<proj>` exists but `op::load_project(proj, ...)` fails: `fail("parse", err)` with the
  loader's own message — do not reword it.
- exit 5 when `save_as` cannot write `<out>` (e.g. its directory does not exist): `fail("write", err)`;
  `<proj>` and `<proj>.autosave` are unchanged.

Deterministic: no clock, no randomness, no environment lookups; the same inputs give byte-identical output
run after run.

### Determinism / repo rules (ADR-0004)

No wall clock, no unseeded randomness, libm only via dsp.hpp, no fast-math; passes `-Werror` and
`-DDZ_SANITIZE=ON`.

## Acceptance criteria

- [x] `opd recover <proj>` with no autosave file prints `recover: <proj>: no autosave file` and exits 0;
      `--restore -o x.opj` in that state exits 3 with `error: not_found: no recoverable autosave for '<proj>'`
      and creates no `x.opj`.
- [x] With `<proj>.autosave` holding different content, the report line is
      `recover: <proj>: autosave differs from the saved project (recoverable)` (exit 0) and `--restore -o x.opj`
      exits 0, writes `x.opj` byte-identical to `<proj>.autosave`, leaves `<proj>` byte-identical, keeps
      `<proj>.autosave`, and `opd info x.opj` succeeds and reports the autosave's values (not the saved file's).
- [x] With `<proj>.autosave` byte-identical to the saved file: report `autosave matches the saved project`
      (exit 0) and `--restore` exits 3 with the same `not_found` message, writing nothing.
- [x] With `<proj>.autosave` not parseable: report `autosave unreadable (corrupt)` (exit 0) and `--restore`
      exits 4 with `error: parse: corrupt autosave '<proj>.autosave'`, writing nothing.
- [x] `<proj>.autosave.tmp` alone is not a recovery state (report says `no autosave file`); `--list` and no
      flag print the same line; `-o` naming `<proj>` itself does overwrite `<proj>` and clear that autosave.
- [x] Usage errors (no `<proj>`, unknown arg, `--list --restore` together, `--restore` without `-o`, `-o`
      without `--restore`) all exit 2 and write nothing; missing `<proj>` exits 3; unparsable `<proj>`
      exits 4; unwritable `-o` path exits 5.
- [x] `tests/test_recover.cpp` with at least 15 CHECKs covering: the four report lines and exit 0 each; the
      two restore error messages with no output file; a successful restore (bytes equal the autosave, project
      file untouched, autosave kept, output loads as a project with the autosave's values); determinism (two
      restores byte-identical); default mode equals `--list`; `.autosave.tmp` ignored; explicit `-o <proj>`
      overwrite + autosave cleared; missing/unparsable project exit codes; all five usage exit codes.
- [x] `src/doc/` and `src/engine/` unchanged; existing goldens bit-identical (`bash harness/run_goldens.sh`);
      `harness/test_cli_errors.sh` still 0 failures.
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_recover_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer
      build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_recover_tests`.

## Out of scope - do NOT touch

- `src/doc/` (no new Document API, no autosave scheduling, no timer, no deletion policy change),
  `src/engine/`, `src/app/`, `ROADMAP.md`, golden fixtures (`tests/fixtures/`), `harness/`, `.github/`,
  `ai-sidecar/`, `tests/test_main.cpp`, `docs/` (the runner already updated `docs/spec/operations.md`).
- No new engine module, no new `src/cli/*.cpp` file, no new library, no JSON output for this command.
- No GUI/app work, no "recover all projects" scan, no `--force`, no mtime-based newer-than logic
  (recovery is content-based: `recovery_state` already decided it), no deletion of autosave files except the
  document layer's own `save_as` behaviour.

## Git
- Branch `wp-0244-opd-recover-inspect-restore` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0244: opd recover — GUI-free crash-recovery report and restore of the autosave file`. Stage explicit paths only (never `git add -A`).
  Include `TASKS/WP-0244-opd-recover-inspect-restore.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

- **What changed — 3 files written, all purely additive (nothing outside the package's scope):**
  - `src/cli/main.cpp` **+71 / −0** (`git diff --numstat src/cli/main.cpp` below proves zero deleted/modified
    lines): `#include "doc/document.hpp"` directly above `#include "engine/analyze.hpp"`; one usage line
    `  opd recover <proj.opj> [--list | --restore -o <out.opj>]   (crash-recovery report / restore)`;
    `static int cmd_recover(int argc, char** argv)` directly before `int main(`; and
    `if (cmd == "recover") return cmd_recover(argc - 1, argv + 1);  // WP-0244` after the `audition` dispatch.
    No new `src/cli/*.cpp` file — `opd` is still a single-file target.
  - `tests/test_recover.cpp` (new, 358 lines, **48 CHECKs**, own `main`, own `CHECK(cond, name)` printing
    `PASS`/`FAIL`, returns the failure count; one scratch dir `temp_directory_path()/op_recover_tests_wp0244_<pid>`,
    removed at the end; the `opd` binary is driven through `OPD_BIN` in the style of `tests/test_tracks.cpp`).
  - `CMakeLists.txt` **+10 / −0**: the 7 specified lines in the 3 specified places, plus one
    `# WP-0244: opd recover — …` comment line above the test block.
  - The runner's `docs/spec/operations.md` section is committed **unchanged** (+60 / −0). `src/doc/`,
    `src/engine/`, `src/app/`, `tests/test_main.cpp`, `tests/fixtures/`, `harness/`, `ai-sidecar/`, `docs/`
    carry no edit of mine — see the `git diff --stat` in the evidence below.
- **How it is implemented (no rule re-decided):** args are parsed in the `cmd_diff` shape (positional `<proj>`,
  `--list` / `--restore` / `-o` in a loop; a second positional or an unknown option is an unknown-arg usage
  error), then the five usage checks; then `fs::exists(proj)` → exit 3 and `op::load_project(proj, …)` → exit 4
  with the loader's message verbatim. The four states come from the document layer only:
  `op::Document::recovery_state(proj) == op::RecoveryState::AutosaveNewer` → recoverable; else
  `!fs::exists(op::Document::autosave_path(proj))` → missing; else `op::load_project` on that file failing →
  corrupt; else identical. Report mode prints one line and returns 0 in all four. Restore mode calls
  `op::Document::open_recovered(proj, doc, err)` then `doc.save_as(out, err)` (never `doc.save()`, never
  `op::save_project`, never a file copy) and does no deleting of its own — the `<proj>.autosave` /
  `<out>.autosave` removals are `save_as`'s rule, which stays observable. No clock, no randomness, no
  environment lookups; `<proj>.autosave.tmp` is never opened, reported or removed.
- **Build:** `bash agent-build-kit/loop/tools/dz_build.sh openpattern --out builds/wp0244 --test`
  (ccache dir `qwen-scratch/.ccache`, `-j4`, `-Werror` on our targets).

```
$ bash agent-build-kit/loop/tools/dz_build.sh /home/.../openpattern --out .../builds/wp0244 --test   # exit 0
$ grep -ci warning .../builds/wp0244/dz_build.log
0
$ cd .../builds/wp0244 && ctest --output-on-failure -j4 | tail -6
      Start  4: lint
18/19 Test  #4: lint .............................   Passed    0.00 sec
19/19 Test  #1: unit .............................   Passed    0.71 sec

100% tests passed, 0 tests failed out of 19
Total Test time (real) =   0.71 sec
      (ctest list includes: 19/19 Test #19: recover ..................   Passed)
```

- **`op_recover_tests` (48 CHECKs, all required cases):**

```
$ .../builds/wp0244/op_recover_tests; echo "exit=$?"
PASS recover.fixture_saved_project_writes_bytes
PASS recover.report_missing_exact_line_exit_0
PASS recover.no_flag_equals_list_missing_state
PASS recover.restore_missing_exit_3_exact_stderr
PASS recover.restore_missing_writes_no_output
PASS recover.report_creates_no_files
PASS recover.fixture_writes_differing_autosave
PASS recover.report_recoverable_exact_line_exit_0
PASS recover.no_flag_equals_list_recoverable_state
PASS recover.restore_ok_exact_stdout_exit_0
PASS recover.restore_output_bytes_equal_autosave_bytes
PASS recover.restore_leaves_project_bytes_unchanged
PASS recover.restore_keeps_autosave_file
PASS recover.restore_output_loads_with_autosave_values
PASS recover.opd_info_reports_autosave_values_not_saved_ones
PASS recover.two_restores_byte_identical
PASS recover.fixture_writes_identical_autosave
PASS recover.report_identical_exact_line_exit_0
PASS recover.restore_identical_exit_3_exact_stderr
PASS recover.restore_identical_writes_no_output
PASS recover.restore_identical_keeps_autosave_file
PASS recover.fixture_writes_corrupt_autosave
PASS recover.report_corrupt_exact_line_exit_0
PASS recover.restore_corrupt_exit_4_exact_stderr
PASS recover.restore_corrupt_writes_no_output
PASS recover.corrupt_state_leaves_project_unchanged
PASS recover.fixture_writes_tmp_file
PASS recover.tmp_only_reports_no_autosave_file
PASS recover.tmp_only_restore_exit_3_writes_nothing
PASS recover.tmp_file_is_never_read_or_removed
PASS recover.tmp_only_leaves_project_unchanged
PASS recover.restore_into_project_itself_exit_0
PASS recover.restore_into_project_itself_writes_autosave_bytes
PASS recover.restore_into_project_itself_clears_that_autosave
PASS recover.missing_project_report_exit_3
PASS recover.missing_project_restore_exit_3_writes_nothing
PASS recover.fixture_writes_garbage_project
PASS recover.unparsable_project_report_exit_4_loader_message
PASS recover.unparsable_project_restore_exit_4_writes_nothing
PASS recover.unwritable_output_exit_5
PASS recover.write_failure_leaves_project_and_autosave_unchanged
PASS recover.usage_no_project_exit_2
PASS recover.usage_unknown_arg_exit_2
PASS recover.usage_list_and_restore_exit_2
PASS recover.usage_restore_without_o_exit_2
PASS recover.usage_o_without_restore_exit_2
PASS recover.usage_errors_write_no_output_file
PASS recover.report_two_runs_byte_identical
ALL PASS (0 failures)
exit=0
```

- **Acceptance criteria, end-to-end at the terminal** (fresh `opd new p.opj --bpm 120 --track K:sine`;
  `p.opj.autosave` faked with `cp` from an `opd apply`-ed copy, since only the app's `Document::autosave()`
  writes it — the doc layer has no CLI/timer entry point, WP-0157):

```
--- 1. no autosave ---
$ opd recover p.opj
recover: p.opj: no autosave file
exit=0
$ opd recover p.opj --restore -o x.opj
error: not_found: no recoverable autosave for 'p.opj'
exit=3
x.opj NOT created

--- 2. autosave differs (recoverable) ---
ede38e10acdee6e0943a337c6808aeeaad1e181dfe0f6a968f542577872e0dec  p.opj
b078f592b2e0b045d90376aed1b18f1ce2b2a570cc5d6a54eaa8e8685ce176b6  p.opj.autosave
$ opd recover p.opj
recover: p.opj: autosave differs from the saved project (recoverable)
exit=0
$ opd recover p.opj --list
recover: p.opj: autosave differs from the saved project (recoverable)
exit=0
$ opd recover p.opj --restore -o x.opj
recover: p.opj: restored p.opj.autosave -> x.opj
exit=0
x.opj == p.opj.autosave (byte-identical)          # cmp x.opj p.opj.autosave
ede38e10acdee6e0943a337c6808aeeaad1e181dfe0f6a968f542577872e0dec  p.opj      # unchanged
b078f592b2e0b045d90376aed1b18f1ce2b2a570cc5d6a54eaa8e8685ce176b6  p.opj.autosave   # kept
b078f592b2e0b045d90376aed1b18f1ce2b2a570cc5d6a54eaa8e8685ce176b6  x.opj
$ opd info x.opj
project: x.opj bpm=137.50 bars=4.00 tracks=1 patterns=1 render=335128 frames (6.982s) seed=42
  [0] K
exit=0
$ opd info p.opj
project: p.opj bpm=120.00 bars=4.00 tracks=1 patterns=1 render=384000 frames (8.000s) seed=42
  [0] K
exit=0
two restores byte-identical                        # cmp x.opj x2.opj

--- 3. autosave identical to the saved file ---
$ opd recover p.opj
recover: p.opj: autosave matches the saved project
exit=0
$ opd recover p.opj --restore -o y.opj
error: not_found: no recoverable autosave for 'p.opj'
exit=3
y.opj NOT created

--- 4. corrupt autosave ---
$ opd recover p.opj
recover: p.opj: autosave unreadable (corrupt)
exit=0
$ opd recover p.opj --restore -o z.opj
error: parse: corrupt autosave 'p.opj.autosave'
exit=4
z.opj NOT created

--- 5. .autosave.tmp alone is not a recovery state ---
$ opd recover p.opj
recover: p.opj: no autosave file
exit=0
tmp file still present

--- 6. -o naming the project itself: overwrite + that autosave cleared ---
$ opd recover p.opj --restore -o p.opj
recover: p.opj: restored p.opj.autosave -> p.opj
exit=0
b078f592b2e0b045d90376aed1b18f1ce2b2a570cc5d6a54eaa8e8685ce176b6  p.opj     # now the autosave's bytes
b078f592b2e0b045d90376aed1b18f1ce2b2a570cc5d6a54eaa8e8685ce176b6  alt.opj   # same content
p.opj.autosave REMOVED by save_as

--- 7. usage / not-found / parse / write errors (each run also prints the full usage block first;
        elided here and marked [usage…]) ---
$ opd recover
[usage…]
error: usage: recover needs <proj.opj> [--list | --restore -o <out.opj>]
exit=2
$ opd recover p.opj --nope
[usage…]
error: usage: unknown arg '--nope'
exit=2
$ opd recover p.opj --list --restore -o u.opj
[usage…]
error: usage: --list and --restore are mutually exclusive
exit=2
$ opd recover p.opj --restore
[usage…]
error: usage: recover --restore needs -o <out.opj>
exit=2
$ opd recover p.opj -o u.opj
[usage…]
error: usage: -o needs --restore
exit=2
u.opj NOT created
$ opd recover nope.opj
error: not_found: nope.opj
exit=3
$ opd recover bad.opj
error: parse: json: [json.exception.parse_error.101] parse error at line 1, column 2: syntax error while parsing value - invalid literal; last read: 'th'
exit=4
$ opd recover p.opj --restore -o no_such_dir/out.opj        # recoverable state, unwritable -o
error: write: cannot write 'no_such_dir/out.opj'
exit=5
ede38e10acdee6e0943a337c6808aeeaad1e181dfe0f6a968f542577872e0dec  p.opj      # unchanged
b078f592b2e0b045d90376aed1b18f1ce2b2a570cc5d6a54eaa8e8685ce176b6  p.opj.autosave   # unchanged
no_such_dir/out.opj NOT created
$ opd recover p.opj other.opj
[usage…]
error: usage: unknown arg 'other.opj'
exit=2
```

- **Goldens / CLI contract / glue e2e — still clean:**

```
$ bash harness/run_goldens.sh .../builds/wp0244/opd | tail -3
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures
(the 16 lines above it all read "PASS <fixture>: deterministic + golden match" — bit-exact, goldens untouched)

$ bash harness/test_cli_errors.sh .../builds/wp0244/opd | tail -3
PASS samples_o_file_matches_stdout (stdout empty)
test_cli_errors: 0 failures

$ cp .../builds/wp0244/opd build/opd && UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv \
    bash harness/test_glue_scripts.sh | tail -3     # build/opd removed again afterwards
PASS rel_no_repo_strays
test_glue_scripts: 0 failures
(21 PASS lines, exit 0)
```

- **Sanitizers (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_recover_tests`:**

```
$ DZ_BUILD_TYPE=Debug bash agent-build-kit/loop/tools/dz_build.sh .../openpattern --sanitize \
      --target op_tests --target op_recover_tests --out .../builds/wp0244-san-dbg     # exit 0, 0 errors
$ grep -m1 CMAKE_BUILD_TYPE: .../builds/wp0244-san-dbg/CMakeCache.txt
CMAKE_BUILD_TYPE:STRING=Debug
$ .../builds/wp0244-san-dbg/op_recover_tests; echo "exit=$?"
… PASS recover.report_two_runs_byte_identical
ALL PASS (0 failures)   [48 PASS / 0 FAIL]  exit=0     stderr: 0 bytes (no ASAN/UBSAN report)
$ .../builds/wp0244-san-dbg/op_tests; echo "exit=$?"
… PASS pmode.failure_leaves_out_untouched
ALL PASS (0 failures)   [1074 PASS / 0 FAIL]  exit=0    stderr: 0 bytes (no leak report)
```

  Note on the sanitizer build type: with `CMAKE_BUILD_TYPE=Release` this toolchain's gcc 13.3 hits a
  pre-existing `-Werror=array-bounds` false positive in `src/engine/render.cpp:786`
  (`out.arrangement = {clip};` under ASAN). It is unrelated to this package (no engine file is touched) and
  reproduces on the base tree — the repo's own `build-san-wp0211/dz_build.log` and `build-san-wp0227/dz_build.log`
  show the identical failure; every successful sanitizer run here (`build-san-wp0227v2`, …) used `Debug`,
  so that is what this package used too. `PROPOSE:` an ADR/repo fix for that engine warning (e.g. a targeted
  `#pragma GCC diagnostic` or an explicit `std::vector<Clip>{clip}`) as its own package — not touched here.

- **Diff shape (scope proof):**

```
$ git diff --numstat | cat
10	0	CMakeLists.txt
60	0	docs/spec/operations.md      # installed by the runner, committed verbatim
71	0	src/cli/main.cpp
$ git diff --stat src/engine src/doc src/app tests/test_main.cpp tests/fixtures harness | cat
(no output — exit 0: nothing under src/engine, src/doc, src/app, tests/test_main.cpp, tests/fixtures or
 harness was touched; the only docs/ diff is the runner's own spec section)
```

- **Metrics:** 48 new CHECKs (≥15 required); `opd` binary size unchanged category (one more TU-level
  function, +71 lines of CLI code); new ctest case `recover` (0.05 s), full `ctest -j4` still 19/19 in 0.71 s;
  no new targets beyond `op_recover_tests`; no new dependency (`opd` now links the existing `dzdoc` target).
- **Follow-ups / notes for the human:**
  - No demo artifact attached: this package writes no audio (report/restore of `.opj` bytes only); the
    goldens are bit-identical and `opd info` evidence is above.
  - `docs/spec/operations.md` was not edited (the runner's section is the contract, committed unchanged).
  - The repo's leftover untracked `wpdraft6/` directory (draft-kit scratch, not created by this package) was
    deliberately **not** staged or deleted — stage list was the 4 explicit paths only.
  - No `PROPOSE:` for the CLI itself. Only the sanitizer/-Werror engine note above.

