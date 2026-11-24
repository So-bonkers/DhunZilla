---
id: WP-0246
title: "`opd new --time-signature N/D` — a new project starts in that meter, byte-identical to `opd new` + the set_bpm / set_time_signature / set_length_bars ops"
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [cli:main, cmake, spec:project-model, new:tests/test_new_opts.cpp]
---

## Context - why this exists

`opd new` already takes `--bpm`, `--bars`, `--beats-per-bar`, `--sample-rate` and `--track` (WP-0165), but there is
no way to start a project in 6/8 or 3/4: the only lever is `--beats-per-bar`, which cannot express a `den != 4`
meter at all (that is the `time_signature` key added by WP-0191). This package adds `opd new --time-signature N/D`
and pins the rule that makes transport/project setup scriptable: `opd new` with these flags must write **exactly**
the bytes you get from a plain `opd new` followed by the `set_bpm`, `set_time_signature` and `set_length_bars`
operations (byte-for-byte). `--bpm` and `--bars` already satisfy that rule today — they are covered by the new tests
as regression anchors, not rewritten.

Existing code this package reuses (read it first, do not reimplement or modify it): the `set_time_signature` op in
`src/engine/ops.cpp` (`apply_named`) and its applier `op::apply_op` in `src/engine/ops.hpp` (already `#include`d by
`src/cli/main.cpp`); the time-signature range/derivation/normalisation rules in `project_from_json`
(`src/engine/model.cpp`); `op::new_project` / `op::project_bytes` / `op::save_project` (`src/engine/model.hpp`);
the `opd new` option machinery in `cmd_new` (`src/cli/main.cpp`) and its helpers `parse_decimal`, `fail`,
`need_value`. **The derivation and the "store only when `den != 4`" rule must stay in the model: apply the op, do not
compute or write the JSON yourself.**

## Spec - normative (this task file is the contract)

### Files (exactly these 3; stage no others)
- `src/cli/main.cpp` — purely additive **except one permitted edit**: the line
  `const op::Project p = op::new_project(spec);` inside `cmd_new` may drop its `const`. Add, and nothing else:
  1. one new line in `usage()` immediately after the `opd new` line that ends
     `[--track NAME:KIND[:PATH]]... [--force]   (KIND = sine|synth|sampler|metronome)`, exactly:
     `          [--time-signature N/D]   (num/den, exactly like the set_time_signature op)`
     (the hidden check greps the fixed string `[--time-signature N/D]` in `src/cli/main.cpp`);
  2. one file-static helper immediately before `static int cmd_new(`:
     `static bool parse_time_signature_arg(const std::string& v, int& num, int& den, std::string& msg);`
     — shape + range validation only (see the table of messages below); it does not touch a project;
  3. inside `cmd_new`: local state for the flag (`bool has_ts`, `int ts_num`, `int ts_den`, the raw value for
     messages, and a flag recording whether `--beats-per-bar` was given), one `else if (opt == "--time-signature")`
     branch in the existing option chain, the `--beats-per-bar`/`--time-signature` conflict check placed after the
     option loop and **before** the output path is examined, and after `op::new_project` the application of the op.
  Do NOT create a new CLI source file; `opd` is a single-file target.
- `tests/test_new_opts.cpp` (new) — own `main`, own `CHECK(cond, name)` printing `PASS <name>` / `FAIL <name>`;
  unique temp dir (`std::filesystem::temp_directory_path()` + a name containing the pid), never fixed `/tmp/op_*`
  paths; remove it at the end; run the CLI through the `OPD_BIN` macro (copy the runner style of
  `tests/test_tracks.cpp`); do NOT edit `tests/test_main.cpp`.
- `CMakeLists.txt` — purely additive: these five lines (under a `# WP-0246: ...` comment) immediately after the line
  `add_test(NAME tracks COMMAND op_tracks_tests)`, then a blank line:
  ```cmake
  add_executable(op_new_opts_tests tests/test_new_opts.cpp)
  target_link_libraries(op_new_opts_tests PRIVATE opengine)
  target_compile_definitions(op_new_opts_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
  add_dependencies(op_new_opts_tests opd)
  add_test(NAME new_opts COMMAND op_new_opts_tests)
  ```
  plus `list(APPEND DZ_OUR_TARGETS op_new_opts_tests)  # WP-0246` immediately after
  `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227`.
- `docs/spec/project-model.md` — **do not edit it**: the runner installs the updated spec file for you (new section
  `## New project time signature — additive (WP-0246)`), and it must match what you implemented.

### Interface (exact signatures, CLI syntax, JSON keys, error strings, exit codes)

**Syntax** (one new flag on the existing command; every other flag keeps its current behaviour and message):

    opd new <out.opj> [--bpm X] [--bars N] [--beats-per-bar N] [--time-signature N/D] [--sample-rate SR]
            [--track NAME:KIND[:PATH]]... [--force]

**Value grammar (normative).** The value is `<N>/<D>`. The **first** `/` splits it; `N` is everything before it,
`D` everything after it, so `6/8/4` is invalid. Both sides are ASCII decimal digits only (leading zeros allowed,
`04/08` is `4/8`), parsed with the same digit parser `--bars` uses (`parse_decimal`); a sign, a decimal point,
spaces or any other character make that side invalid. Ranges, all four checked in this order:
1. no `/` in the value → shape error;
2. `N` must be an integer with `1 <= N <= 32`;
3. `D` must be one of `2`, `4`, `8`, `16`;
4. `(N * 4) % D == 0` (the bar must be a whole number of quarter-note beats: `7/8`, `5/8`, `9/16` are rejected) and
   `N * 4 / D <= 32` (`32/2` = 64 beats is rejected).

**Effect (normative, this is the whole point).** `opd new <out> --time-signature N/D` (together with any other
flags) writes EXACTLY the bytes produced by `opd new <out2>` (no signature flag) followed by the operation
`{"op":"set_time_signature","num":N,"den":D}` applied through the existing op machinery. Consequences:
- `beats_per_bar` is always re-derived as `N * 4 / D` (`6/8` → 3, `3/4` → 3, `2/2` → 4, `32/4` → 32);
- the saved JSON holds a `"time_signature": { "den": D, "num": N }` object **iff `D != 4`**; `den == 4` normalizes
  away, so `--time-signature 3/4` and `--time-signature 32/4` write **no** `time_signature` key, while `6/8`, `2/2`
  and `12/16` do (note `2/2` derives 4 and is still stored — do not normalize on the derived value);
- `--time-signature 4/4` writes byte-identical bytes to no flag at all.

Implement it by calling the existing applier, e.g.
`op::apply_op(p, nlohmann::json{{"op", "set_time_signature"}, {"num", ts_num}, {"den", ts_den}}, err)` after
`op::new_project`. Banned: hand-writing the key, hand-copying the model's range rules into the project build, any
edit under `src/engine/`, any second ruleset. If `apply_op` ever rejects a value your checks accepted, exit 2 with
`error: usage: bad --time-signature value '<v as given>'` (unreachable by construction; do not add other paths).

**Byte-equality table (the hidden check runs every row).** For each row, `opd new a.opj --bpm B --time-signature S
--bars K` must equal `opd new b.opj` + `opd apply b.opj <journal> -o r.opj` where `<journal>` is exactly these
three lines, in this order: `{"op":"set_bpm","bpm":B}`, `{"op":"set_time_signature","num":N,"den":D}`,
`{"op":"set_length_bars","length_bars":K}`.

| B | S | K |
|---|---|---|
| 90.0 | 6/8 | 7 |
| 140.5 | 3/4 | 1 |
| 120.0 | 2/2 | 16 |
| 72.0 | 12/16 | 4096 |
| 0.5 | 32/4 | 2 |
| 999.99 | 4/4 | 8 |
| 121.0 | 12/8 | 33 |
| 119 | 1/2 | 4 |

**Combinations (normative).** Options may appear in any order; a repeated `--time-signature` keeps the last value
(like every other option). `--beats-per-bar M` may be combined with `--time-signature N/D` only when
`M == N * 4 / D`; then the bytes are exactly those of `--time-signature N/D` alone. If `M != N * 4 / D`: exit 2,
`error: usage: --time-signature '<v as given>' conflicts with --beats-per-bar <M> (beats_per_bar would be <N*4/D>)`,
nothing written — and that check runs after the whole option loop, so an option-level error on either flag is
reported first no matter which order the flags appear in. An error never writes the file (existing rule).

**Errors (exact strings; `exit 2`, on stderr after the usage block, nothing written).** `<v as given>` is the raw
argument. Each row is the complete error line:

| run | error line |
|---|---|
| `--time-signature abc` | `error: usage: bad --time-signature value 'abc': expected N/D (e.g. 6/8)` |
| `--time-signature 6x8` | `error: usage: bad --time-signature value '6x8': expected N/D (e.g. 6/8)` |
| `--time-signature 6/8/4` | `error: usage: bad --time-signature value '6/8/4': den must be 2, 4, 8 or 16` |
| `--time-signature /8` | `error: usage: bad --time-signature value '/8': num must be an integer in [1, 32]` |
| `--time-signature 6/` | `error: usage: bad --time-signature value '6/': den must be 2, 4, 8 or 16` |
| `--time-signature 7/8` | `error: usage: bad --time-signature value '7/8': bar must be a whole number of quarter-note beats` |
| `--time-signature 4/3` | `error: usage: bad --time-signature value '4/3': den must be 2, 4, 8 or 16` |
| `--time-signature 0/4` | `error: usage: bad --time-signature value '0/4': num must be an integer in [1, 32]` |
| `--time-signature 33/4` | `error: usage: bad --time-signature value '33/4': num must be an integer in [1, 32]` |
| `--time-signature 32/2` | `error: usage: bad --time-signature value '32/2': bar must be at most 32 quarter-note beats` |
| `--time-signature` (no value) | `error: usage: missing value for --time-signature` |
| `--time-signature 3/4 --beats-per-bar 5` | `error: usage: --time-signature '3/4' conflicts with --beats-per-bar 5 (beats_per_bar would be 3)` |
| `--beats-per-bar 5 --time-signature 3/4` | `error: usage: --time-signature '3/4' conflicts with --beats-per-bar 5 (beats_per_bar would be 3)` |

Must be **accepted** (exit 0): `1/2`, `12/16`, `32/4`, `4/8`, `04/08` (same bytes as `4/8`), `2/2`, `3/4`, `6/8`,
`4/4`. Boundaries `N = 1`, `N = 32`, `D = 2`, `D = 16` are valid; `N = 0`, `N = 33`, `D = 1`, `D = 3`, `D = 32` are
not. `--bpm`, `--bars`, `--beats-per-bar`, `--sample-rate`, `--track`, `--force` keep their current ranges,
messages and left-to-right validation order (docs/spec/project-model.md §New project) — do not touch them.

**Unchanged output contract.** On success: exit 0, stdout exactly
`new: <out.opj> tracks=<n> bars=<n> bpm=<%g of bpm>` — do NOT add the signature to that line, and do not change
`project_bytes`, the goldens or any other command.

### Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, libm only via dsp.hpp, no fast-math; passes `-Werror` and
`-DDZ_SANITIZE=ON`. Tests use a unique temp dir (`std::filesystem::temp_directory_path()` + pid), never fixed
`/tmp/op_*` paths, and remove it at the end.

## Acceptance criteria
- [ ] Everything above implemented; the 8 byte-equality rows compare equal; existing goldens bit-identical
      (`bash harness/run_goldens.sh`).
- [ ] Defaults unchanged: `opd new <out>` with no flags writes the same bytes as before (no `time_signature` key,
      `"beats_per_bar": 4`, `"bpm": 120.0`, `"length_bars": 4.0`), and `--time-signature 4/4` is byte-identical to it.
- [ ] `tests/test_new_opts.cpp` with at least 30 CHECKs covering: every byte-equality row (CLI output vs
      `op::project_bytes` of the same three ops applied by `op::apply_op`, or vs `opd apply` of a journal);
      `4/4` == default bytes; `3/4` and `32/4` write no `time_signature` key and derive `beats_per_bar` 3 / 32;
      `6/8`, `2/2`, `12/16` store `num`/`den` and derive 3 / 4 / 3; the written file is canonical (its bytes equal
      `op::project_bytes` of the loaded project) and round-trips through `opd apply` of an empty journal;
      leading zeros (`04/08` == `4/8`) and the accepted boundaries; every error row above (exit 2, exact message,
      no file written); the conflict rule in both orders and the consistent-`--beats-per-bar` case; last
      `--time-signature` wins; flag order independence; composing with `--track`/`--sample-rate`; the existing
      `--bars 0`, `--bpm 0`, `--beats-per-bar 33` and `--sample-rate` messages still exact; the success line
      unchanged and two identical runs byte-identical.
- [ ] `opd new` exit codes/stdout as specified, nothing written on any error.
- [ ] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_new_opts_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_new_opts_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (do not edit it), `docs/` (the runner installs the spec update), `tests/fixtures/` (goldens),
  `harness/`, `.github/`, `ai-sidecar/`, `tests/test_main.cpp`, and **everything under `src/engine/`** — the
  rules already live there; this package only calls `op::apply_op`.
- No new CLI source file, no new engine module, no new operation, no changes to `set_time_signature`,
  `set_bpm`, `set_length_bars`, `opd diff`, `opd apply`, `opd timeline`, `opd info` or `project_bytes`.
- No tempo map / loop-region / sample-rate flags, no `--time-signature` on any other subcommand, no JSON written
  by hand, no wall-clock or randomness anywhere.

## Git
- Branch `wp-0246-opd-new-bpm-time` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0246: opd new --time-signature — new projects start in that meter`.
  Stage explicit paths only (never `git add -A`).
  Include `TASKS/WP-0246-opd-new-bpm-time.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

**Branch:** `wp-0246-opd-new-bpm-time` (from the integration base `d7e3fa3`). Not pushed, not merged.

### What changed (exactly the 3 code files + the runner-installed spec + this task file)
- `src/cli/main.cpp` — purely additive except the one permitted edit (`const op::Project p = op::new_project(spec);`
  → `op::Project p`): the new usage line with the literal `[--time-signature N/D]`, the file-static
  `parse_time_signature_arg()` (shape/range checks + exact messages only), cmd_new local state (`has_ts`, `ts_arg`,
  `ts_num`, `ts_den`, `has_bpb`, `bpb_value`), the `--time-signature` option branch, the `--beats-per-bar` conflict
  check placed after the option loop and before the output path is examined, and after `op::new_project` the
  application of `op::apply_op(p, {{"op","set_time_signature"},{"num",ts_num},{"den",ts_den}}, err)`. The CLI owns
  no derivation and writes no JSON; the `apply_op`-rejects fallback exits 2 with
  `error: usage: bad --time-signature value '<v as given>'`.
  Verified: `git diff src/cli/main.cpp | grep "^-"` prints only the diff header and that one permitted line.
- `tests/test_new_opts.cpp` (new) — own `main`, own `CHECK`, per-process temp dir
  (`temp_directory_path()/op_new_opts_tests_wp0246_<pid>`, removed at the end), CLI via `OPD_BIN`, runner style of
  `test_tracks.cpp`. **58 CHECKs** (≥30 required): all 8 byte-equality rows twice (vs `op::project_bytes` of the
  three `apply_op` ops AND vs `opd apply` of the exact 3-line journal); defaults unchanged; `4/4` == default bytes;
  `3/4`/`32/4` no key + derive 3/32; `6/8`/`2/2`/`12/16` store num/den + derive 3/4/3 and `2/2` ≠ bare
  `--beats-per-bar 4`; canonical bytes == `project_bytes(load(file))` and empty-journal `opd apply` round trip;
  `04/08` == `4/8`; accepted boundaries (1/2, 1/4, 4/16, 32/4, 32/16, 16/2 + the listed meters); all 13 error rows
  (exit 2, exact complete line, empty stdout, no file written); conflict both orders + consistent `--beats-per-bar`
  (den≠4 and den=4); last `--time-signature` wins; flag order independence (options before/after the positional);
  composing with `--track`/`--sample-rate`; existing `--bars 0`, `--bpm 0`, `--beats-per-bar 33`,
  `--sample-rate 7999` messages exact; option-level error precedence in both orders; success line unchanged;
  two identical runs byte-identical.
- `CMakeLists.txt` — the five specified lines under a `# WP-0246:` comment immediately after
  `add_test(NAME tracks COMMAND op_tracks_tests)`, plus `list(APPEND DZ_OUR_TARGETS op_new_opts_tests)  # WP-0246`.
- `docs/spec/project-model.md` — included **unchanged** as installed by the runner (section
  `## New project time signature — additive (WP-0246)` matches this implementation; spec §`--time-signature` bullet,
  grammar, normalization, errors and conflict rule all verified by the tests above).
- Nothing under `src/engine/`, `harness/`, `tests/fixtures/`, `tests/test_main.cpp`, `ai-sidecar/` touched. No goldens
  or fixtures changed.

### How verified (real output)

**1. Build — dz-build skill (`-j4`, lock, ccache; `-Werror` in every DZ_OUR_TARGET's flags; 0 warnings):**
```
$ bash agent-build-kit/loop/tools/dz_build.sh "$PWD" --out "$PWD/build-wp0246"   # (all targets)
/home/shubhankar_dgx/qwen-scratch/openpattern/build-wp0246
$ grep -ci warning build-wp0246/dz_build.log
0
$ grep -o -- "-Werror" build-wp0246/CMakeFiles/op_new_opts_tests.dir/flags.make
-Werror
```

**2. Full ctest (includes `unit`, `tracks` and the new `new_opts`):**
```
100% tests passed, 0 tests failed out of 19
```
`op_new_opts_tests` directly: `58` PASS lines, `0` FAIL lines, `ALL PASS (0 failures)`, exit 0. Tail:
```
PASS option_level_error_wins_ts_left_first
PASS option_level_error_wins_bpb_left_first
PASS success_line_unchanged_no_signature
PASS two_identical_runs_byte_identical
ALL PASS (0 failures)
```

**3. Byte-equality row 1 by hand (the whole point of the package):**
```
$ ./opd new $T/a.opj --bpm 90.0 --time-signature 6/8 --bars 7
new: /tmp/tmp.oEyjeaKCX3/a.opj tracks=1 bars=7 bpm=90
$ ./opd new $T/b.opj ; printf '{"op":"set_bpm","bpm":90.0}\n{"op":"set_time_signature","num":6,"den":8}\n{"op":"set_length_bars","length_bars":7}\n' > $T/j.opjl
new: /tmp/tmp.oEyjeaKCX3/b.opj tracks=1 bars=4 bpm=120
$ ./opd apply $T/b.opj $T/j.opjl -o $T/r.opj
apply: 3 op(s) -> /tmp/tmp.oEyjeaKCX3/r.opj
$ cmp $T/a.opj $T/r.opj && echo BYTE-IDENTICAL
BYTE-IDENTICAL
```
Error paths by hand (the test suite asserts exit code 2 exactly via waitpid):
```
$ ./opd new $T/x.opj --time-signature 32/2      → error: usage: bad --time-signature value '32/2': bar must be at most 32 quarter-note beats
$ ./opd new $T/x.opj --time-signature 3/4 --beats-per-bar 5
                                                 → error: usage: --time-signature '3/4' conflicts with --beats-per-bar 5 (beats_per_bar would be 3)
$ ls $T/x.opj                                    → No such file or directory (nothing written on any error)
```

**4. `bash harness/run_goldens.sh build-wp0246/opd` — existing goldens bit-identical:**
```
PASS time_sig_3_4: deterministic + golden match
PASS time_sig_6_8: deterministic + golden match
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures
```

**5. `bash harness/test_cli_errors.sh build-wp0246/opd`:**
```
test_cli_errors: 0 failures
```

**6. `bash harness/test_glue_scripts.sh` (with `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`;
`build/opd` symlinked to the WP binary for the run, removed after):**
```
PASS determinism (opj byte-identical)
PASS rel_no_repo_strays
test_glue_scripts: 0 failures
```

**7. Sanitizer build (`-DDZ_SANITIZE=ON`, ASAN+UBSAN) of `op_tests` and `op_new_opts_tests`:**
```
$ DZ_BUILD_TYPE=Debug bash agent-build-kit/loop/tools/dz_build.sh "$PWD" --out "$PWD/build-san-wp0246" --sanitize \
      --target op_tests --target op_new_opts_tests
$ ./build-san-wp0246/op_tests        → ALL PASS (0 failures)
$ ./build-san-wp0246/op_new_opts_tests → ALL PASS (0 failures)   # runs the sanitizer-built opd binary as a subprocess too
```
Note (pre-existing, NOT introduced here): with `dz_build.sh`'s default `DZ_BUILD_TYPE=Release` the sanitizer build
fails in `src/engine/render.cpp:786` (`-Werror=array-bounds` GCC 13.3 false positive on
`out.arrangement = {clip};` — `__builtin_memmove … forming offset 72 is out of the bounds`). Reproduced identically
on the untouched base `d7e3fa3` in a throwaway worktree, so it predates this package; earlier sanitizer dirs
(`build-san-wp0215`, `build-san-wp0227v2`) were all configured `CMAKE_BUILD_TYPE=Debug`, which passes.
`PROPOSE:` a follow-up WP to pin the sanitizer gate to the Debug configuration (or raise the GCC version) — the fix
lives in `src/engine/`, out of this package's scope.

### Metrics
- 58 CHECKs in `tests/test_new_opts.cpp`; all 8 byte-equality rows compared twice (apply_op reference + `opd apply` journal).
- `src/cli/main.cpp`: +57/−1 lines (the single deletion is the permitted `const` line); CMake +8 lines.

### Follow-ups / notes
- `PROPOSE:` sanitizer-gate build type (see §7 above).
- The untracked `wpdraft6/` directory (a nested clone of another draft run) was left in place untouched — out of
  this package's scope, not committed; likewise the pre-existing `build*/` dirs (gitignored).

