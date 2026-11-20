---
id: WP-0160
title: op_tests use a per-process temp directory instead of fixed /tmp paths
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
---

## Context — why this exists
`tests/test_main.cpp` writes ~180 fixed paths such as `/tmp/op_test_good.opj`, `/tmp/op_rt_a.mid`,
`/tmp/op_bd/...` and renders with base dir `/tmp`. When two `op_tests` processes run at the same time
(CI matrix, parallel agents, release + sanitizer builds) they overwrite and delete each other's files and
tests flake. This package is a **mechanical, test-only** change: every scratch path goes through one helper
that places it in a directory unique to the process. No test logic, CHECK name or CHECK count changes.

## Spec — normative
Add these two helpers near the top of `tests/test_main.cpp`, after the `CHECK` macro and before
`write_text` (add `#include <unistd.h>` to the includes):

```cpp
// Per-process scratch directory: <temp_directory_path()>/op_tests_<pid>. Created (after removing any
// stale one with that name) on first call; the same string is returned on every later call.
static std::string tmp_dir();

// Path of `name` inside tmp_dir(): tmp_dir() + "/" + name. `name` may contain '/' (e.g. "op_bd/kits");
// the helper does NOT create sub-directories — tests keep creating their own, as they do today.
static std::string tmp_file(const std::string& name);
```

- `tmp_dir()` = `std::filesystem::temp_directory_path() / ("op_tests_" + std::to_string(static_cast<long>(::getpid())))`,
  built once in a function-local `static const std::string`; it calls `remove_all` then `create_directories`
  (error-code overloads, errors ignored) on first use.
- **Cleanup:** at the end of `main`, after the final `ALL PASS` / `FAILURES` line is printed: if
  `g_failures == 0`, `std::filesystem::remove_all(tmp_dir(), ec)` (error-code overload); otherwise leave
  the directory in place and print one extra line `kept scratch dir <tmp_dir()>`. The final
  `ALL PASS (0 failures)` / `FAILURES (n failures)` line and the process exit code stay exactly as today.
- **Every fixed scratch path in `tests/test_main.cpp` goes through the helpers**:
  - each string literal `"/tmp/<name>"` becomes `tmp_file("<name>")` (e.g. `"/tmp/op_test_good.opj"` ->
    `tmp_file("op_test_good.opj")`, `"/tmp/op_bd/kits"`-style sub-paths -> `tmp_file("op_bd/kits")`);
    concatenated forms keep working, e.g. `std::string("/tmp/op_projbytes_") + name + ".opj"` ->
    `tmp_file(std::string("op_projbytes_") + name + ".opj")`;
  - each `"/tmp"` used as a render base dir (`op::render_project(p, "/tmp", ...)`) becomes `tmp_dir()`;
  - the one existing `fs::temp_directory_path() / "op_doc_autosave_wp0157"` in `test_document_autosave`
    becomes `fs::path(tmp_file("op_doc_autosave_wp0157"))`. After this change the only remaining
    `temp_directory_path` call in the file is the one inside `tmp_dir()`;
  - the "definitely missing" file `"/tmp/definitely_missing_file.mid"` becomes
    `tmp_file("definitely_missing_file.mid")` (it must stay nonexistent).
- `tmp_file` returns `std::string`, but several local lambdas take `const char*` parameters and are called
  with these paths. Change those lambda parameters to `const std::string&` (they are:
  `slurp` in `test_midi`, `copyfile` in `test_attachments`, `rb` in `test_arrangement_export`,
  `bytes_of` in `test_document`; the compiler lists any others). Do not change anything else in them.
  Do not name anything `tmp_path` (a local of that name already exists in `test_document_autosave`).
- Comments that mention `/tmp` may stay as they are.

## Acceptance criteria
- [x] `grep -c '"/tmp' tests/test_main.cpp` prints `0`, and `grep -v '^[[:space:]]*//' tests/test_main.cpp | grep -c temp_directory_path`
      prints `1` (comment lines may mention it). → evidence in Handoff §A.
- [x] `tmp_dir()` and `tmp_file()` exist with exactly the signatures above; `tmp_file(` is used at least 60 times. → Handoff §A.
- [x] `op_tests` still prints the **same set of `PASS <name>` lines** as before the change (same names,
      same count; compare `./build/op_tests | grep '^PASS' | sort` before vs after) and ends with
      `ALL PASS (0 failures)`. Record the PASS count in the handoff. → **687 before, 687 after, diff empty**; Handoff §B.
- [x] After a passing run, `<temp dir>/op_tests_<pid>` no longer exists (check with
      `TMPDIR=$(mktemp -d) ./build/op_tests >/dev/null; ls "$TMPDIR"` printing nothing). → Handoff §C.
- [x] Two `op_tests` processes started at the same time, several times in a row, both pass:
      `for i in 1 2 3 4 5; do ./build/op_tests >/dev/null & ./build/op_tests >/dev/null & wait; done`
      then the same loop with exit codes checked (`wait $p1; wait $p2`). Report the result. → Handoff §D.
- [x] Sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` passes. → Handoff §E.
- [x] Standard gates green: build with 0 warnings (`-Werror`), `op_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`. Build with `-j4` max. → Handoff §F.

## Out of scope — do NOT touch
- Anything except `tests/test_main.cpp` and this task file: no source under `src/`, no fixtures, no
  `harness/`, no `docs/`. Do **not** edit `ROADMAP.md` (the elephant adds rows at ship time).
- Do not add, remove, rename or reorder tests or CHECKs; do not change assertions; do not touch
  `OP_TEST_DATA` fixture paths (those are read-only inputs, not scratch).

## Git
- Branch `wp-0160-test-temp-dirs` from `main`. Commit: `WP-0160: op_tests use a per-process temp directory`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:** `tests/test_main.cpp` only (+216 / −191 lines; no other file in the repo touched).
  - Added `#include <unistd.h>` and the two spec'd helpers, placed after the `CHECK` macro and before
    `write_text`: `static std::string tmp_dir()` — one function-local `static const std::string` holding
    `std::filesystem::temp_directory_path() / ("op_tests_" + std::to_string(static_cast<long>(::getpid())))`,
    with `remove_all` then `create_directories` (error-code overloads, errors ignored) on first use — and
    `static std::string tmp_file(const std::string& name) { return tmp_dir() + "/" + name; }`.
  - Migrated every fixed scratch path: **199** `"/tmp…"` literals in total → **194** path literals became
    `tmp_file("<name>")` (including `"op_bd/kits"`-style sub-paths, the concatenated form now
    `tmp_file(std::string("op_projbytes_") + name + ".opj")`, and `fs::temp_directory_path() / "op_doc_autosave_wp0157"`
    → `fs::path(tmp_file("op_doc_autosave_wp0157"))`), and the **5** bare `"/tmp"` render base dirs
    (`op::render_project(p, "/tmp", …)`) became `tmp_dir()`. `/tmp/definitely_missing_file.mid` →
    `tmp_file("definitely_missing_file.mid")`; it is still never created.
  - Lambda path parameters widened `const char*` → `const std::string&`: `slurp` (test_midi), `copyfile`
    (test_attachments), `rb` (test_arrangement_export), `bytes_of` (test_document). Those were the only four
    needed; nothing else in them changed. No name `tmp_path` introduced.
  - End of `main`, after the `ALL PASS` / `FAILURES` line: on `g_failures == 0`,
    `std::filesystem::remove_all(tmp_dir(), ec)`; otherwise one extra line `kept scratch dir <tmp_dir()>` and
    the directory stays. Summary line text and process exit code are byte-identical to before.
  - No test, CHECK name, CHECK count, assertion, comment requirement or `OP_TEST_DATA` fixture path changed.

- **How verified:** (PASS count before/after, concurrent-run results) — all commands run on the DGX Spark
  (aarch64) from the repo root; builds `-j4`; `UV_CACHE_DIR=<workspace>/.cache/uv` for the glue test.

### §A — no fixed `/tmp` literals left, one helper only
```
$ grep -c '"/tmp' tests/test_main.cpp
0
$ grep -v '^[[:space:]]*//' tests/test_main.cpp | grep -c temp_directory_path
1
$ grep -n 'static std::string tmp_dir\|static std::string tmp_file' tests/test_main.cpp
38:static std::string tmp_dir() {
52:static std::string tmp_file(const std::string& name) { return tmp_dir() + "/" + name; }
$ grep -o 'tmp_file(' tests/test_main.cpp | wc -l
196        # 1 definition + 195 call sites (>= 60 required)
```
The two surviving mentions of `/tmp` are comment lines only (`to /tmp → read back …`, `base_dir /tmp for the
fixture WAVs …`), neither matching `'"/tmp'`.

### §B — PASS set unchanged: 687 before, 687 after
Baseline captured from the pre-change binary (branch `main` build), then the same command after the change:
```
$ ./build/op_tests | grep '^PASS' | sort | wc -l      # before: 687   after: 687
$ diff <workspace>/.wp0160_baseline_pass.txt \
       <workspace>/.wp0160_after_pass.txt && echo "PASS LINES IDENTICAL"
PASS LINES IDENTICAL
$ tail -3 <workspace>/.wp0160_after_out.txt
PASS ops.set_sampler_trim_reject_end_type
INFO sampler trim: slice frames [96,480) checked against untrimmed reference
ALL PASS (0 failures)
exit=0
```

### §C — scratch dir removed after a passing run
```
$ T=$(mktemp -d); TMPDIR=$T ./build/op_tests >/dev/null; echo "op_tests exit=$?"
op_tests exit=0
$ ls "$T"
                                        # ← nothing printed
$ echo "entries=$(ls -A "$T" | wc -l)"
entries=0
```

### §D — concurrent runs (the flake this package fixes)
```
$ for i in 1 2 3 4 5; do ./build/op_tests >/dev/null & ./build/op_tests >/dev/null & wait; echo "iter $i: group rc=$?"; done
iter 1: group rc=0
iter 2: group rc=0
iter 3: group rc=0
iter 4: group rc=0
iter 5: group rc=0

$ for i in 1 2 3 4 5; do ./build/op_tests >$D/a$i.txt & p1=$!; ./build/op_tests >$D/b$i.txt & p2=$!; \
    wait $p1; r1=$?; wait $p2; r2=$?; echo "iter $i: pid $p1 rc=$r1 [$(tail -n1 $D/a$i.txt)] | pid $p2 rc=$r2 [$(tail -n1 $D/b$i.txt)]"; done
iter 1: pid 4 rc=0 [ALL PASS (0 failures)] | pid 5 rc=0 [ALL PASS (0 failures)]
iter 2: pid 8 rc=0 [ALL PASS (0 failures)] | pid 9 rc=0 [ALL PASS (0 failures)]
iter 3: pid 12 rc=0 [ALL PASS (0 failures)] | pid 13 rc=0 [ALL PASS (0 failures)]
iter 4: pid 16 rc=0 [ALL PASS (0 failures)] | pid 17 rc=0 [ALL PASS (0 failures)]
iter 5: pid 20 rc=0 [ALL PASS (0 failures)] | pid 21 rc=0 [ALL PASS (0 failures)]
```
10/10 processes passed; each wrote only into its own `op_tests_<pid>` directory.

### §E — sanitizer build (`-DDZ_SANITIZE=ON`)
```
$ cmake --build build-san -j4          # ASAN+UBSAN, DZ_SANITIZE:BOOL=ON
[100%] Linking CXX executable op_tests
[100%] Built target op_tests           EXIT=0
$ ./build-san/op_tests > out.txt 2>&1; echo "san op_tests exit=$?"
san op_tests exit=0
$ grep -c '^PASS' out.txt              # 687
$ tail -3 out.txt
ALL PASS (0 failures)
$ grep -ci 'runtime error\|ERROR:' out.txt
0                                      # no ASAN/UBSAN reports
```

### §F — standard gates, all green
Fresh Release configure + build (`-Werror` ON for our targets), zero warnings anywhere:
```
$ cmake -S . -B build-wp0160 -DCMAKE_BUILD_TYPE=Release        # configure rc=0
$ cmake --build build-wp0160 -j4                               # build rc=0
$ grep -ci 'warning' build-wp0160_build.log
0
$ ./build-wp0160/op_tests | tail -2 ; echo "fresh op_tests exit=${PIPESTATUS[0]}"
ALL PASS (0 failures)
fresh op_tests exit=0
```
(`build-wp0160/` was scratch and has been deleted; the incremental `build/` rebuild was also warning-free.)
```
$ harness/run_goldens.sh            rc=0    run_goldens: 9 fixtures, 0 failures        (10 PASS lines)
$ harness/test_cli_errors.sh        rc=0    test_cli_errors: 0 failures               (66 PASS lines)
$ UV_CACHE_DIR=<workspace>/.cache/uv harness/test_glue_scripts.sh
                                    rc=0    test_glue_scripts: 0 failures             (21 PASS lines)
```

### Extra evidence — failure branch of the cleanup contract
Verified without touching the committed file: a copy of `tests/test_main.cpp` with one injected
`CHECK(false, "wp0160_manual_failure_probe")` was compiled outside the repo and run with a private `TMPDIR`:
```
FAIL wp0160_manual_failure_probe  (…/probe.cpp:3882)
FAILURES (1 failures)
kept scratch dir /tmp/tmp.ZnqlcH4mCK/op_tests_10
probe exit=1
$ ls -A "$T"
op_tests_10                            # left in place for post-mortem, as specified
```

- **Metrics:** 1 file changed (+216 / −191); 199 fixed `/tmp` literals migrated (194 → `tmp_file`, 5 render
  base dirs → `tmp_dir`); 195 `tmp_file(` call sites; 4 lambda parameters widened to `const std::string&`;
  PASS count 687 before = 687 after; 0 warnings under `-Werror`; all gates rc=0.
- **PROPOSE / BLOCKED:** none. Nothing outside `tests/test_main.cpp` needed changing, and no architecture
  decision was made, so no `docs/spec/` or ADR update applies (test-only scratch paths are not observable
  behaviour of the product).
