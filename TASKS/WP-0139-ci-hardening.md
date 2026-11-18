---
id: WP-0139
title: CI hardening — warnings as errors on our targets + ASAN/UBSAN job
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
An independent review found that AGENTS.md promises gates CI doesn't enforce: "zero new warnings" is
honesty-based (no `-Werror`), and commits claim "ASAN-clean" but no CI job can reproduce that. This
package makes both machine-enforced. It touches only build/CI configuration — no engine behavior.

## Spec
1. `CMakeLists.txt`: new `option(DZ_WERROR "Treat warnings as errors on our targets" ON)`. When ON, add
   `-Werror` to **our** targets only (`opengine`, `opd`, `op_tests` — use `target_compile_options`, never
   global flags, so vendored/third-party code can't break the build).
2. `CMakeLists.txt`: new `option(DZ_SANITIZE "Build with ASAN+UBSAN" OFF)`. When ON, add
   `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined` to compile and
   link options of our targets. Determinism flags (`-ffp-contract=off`) stay; never add fast-math.
3. `.github/workflows/ci.yml`: new job `sanitizers` (`ubuntu-24.04` only) that configures with
   `-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`, builds, then runs `./build/op_tests`,
   `bash harness/test_cli_errors.sh` and `bash harness/run_goldens.sh` with
   `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`.
   **If goldens are not bit-exact under Debug+sanitizers** (possible: different optimization can change
   float results), run goldens only in the existing Release jobs and in the sanitizer job run only
   `op_tests` + `test_cli_errors.sh` — document which in the handoff with the evidence.
4. `AGENTS.md` "Definition of done": replace the claims that CI runs clang-format/clang-tidy/mypy (it
   does not) with the gates that now exist: `-Werror` build, unit tests, goldens, CLI contracts, glue
   scripts, sanitizer job, sidecar pytest + ruff. Keep everything else in AGENTS.md unchanged.

## Acceptance criteria
- [ ] `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j4` → 0 warnings, 0 errors (DZ_WERROR ON by default).
- [ ] Proof `-Werror` is live: temporarily add an unused variable to `src/engine/model.cpp`, show the build
      fails, revert (paste both outputs; do not commit the broken version).
- [ ] `cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san -j4` then
      `./build-san/op_tests` and `bash harness/test_cli_errors.sh` (pointed at build-san if the script
      supports it; otherwise note it) → pass with no sanitizer reports. Paste output tails.
- [ ] Proof the sanitizer is live: a tiny throwaway program or test compiled with the same flags that does
      a heap overflow → ASAN report (paste it); do not commit it.
- [ ] If any real sanitizer finding appears in our code: **stop and write it as BLOCKED** with the report —
      do not fix engine code in this package.
- [ ] ROADMAP.md: WP-0139 row at the top of the table.

## Out of scope — do NOT touch
- Any file under `src/`, `harness/` (except reading), `tests/fixtures/`, `ai-sidecar/`, `docs/spec/`, `docs/adr/`.
- clang-format/clang-tidy/mypy adoption (separate decision).

## Git
- Branch `wp-0139-ci-hardening` from `main`. Commit: `WP-0139: -Werror on our targets + ASAN/UBSAN CI job`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.** Build with `-j4` at most (shared memory with the model server).

---
## Handoff report — completed by the agent
- **What changed:** `CMakeLists.txt`: `DZ_WERROR` (ON) adds `-Werror` and `DZ_SANITIZE` (OFF) adds
  `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined` (compile + link),
  both via `target_compile_options` on `opengine`/`opd`/`op_tests` only; determinism flags unchanged.
  Sanitize also disables ASAN's use-after-return fake stack (GCC `--param=asan-use-after-return=0`,
  Clang `-fsanitize-address-use-after-return=never`): with it on, `test_json_fuzz` (nlohmann lexer at -O0)
  ran >10 min and looked hung (gdb backtrace: `__asan::FakeStack::Allocate` under `lexer::get_string`);
  without it the whole suite takes 6.2 s. `.github/workflows/ci.yml`: `sanitizers` job (ubuntu-24.04,
  Debug + `DZ_SANITIZE=ON`, runs op_tests, CLI contracts, goldens with the spec's ASAN/UBSAN options).
  `AGENTS.md` definition of done now lists only gates CI really runs. `.gitignore`: `build-*/`. ROADMAP row.
- **How verified (aarch64 rig):**
  - Fresh `cmake -B <dir> -DCMAKE_BUILD_TYPE=Release && cmake --build <dir> -j4` → rc 0, `grep -c warning:` = 0.
  - `-Werror` live (copy of the tree, unused variable injected in `src/engine/model.cpp`, not committed):
    `error: unused variable 'unused_var' [-Werror=unused-variable]` … `cc1plus: all warnings being treated as errors`.
  - `cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON && cmake --build build-san -j4` → rc 0, 0 warnings;
    `nm -C build-san/op_tests | grep -c __asan_report` = 12. With the spec's ASAN_OPTIONS/UBSAN_OPTIONS:
    `./build-san/op_tests` → `ALL PASS (0 failures)` in 6.21 s · `test_cli_errors.sh ./build-san/opd` →
    `0 failures` · `run_goldens.sh ./build-san/opd` → `6 fixtures, 0 failures` (goldens are bit-exact under
    Debug+sanitizers here, so the CI job runs all three; x86_64 Debug is proven by the first CI run).
  - Sanitizer live (throwaway heap overflow, same flags, not committed):
    `ERROR: AddressSanitizer: heap-buffer-overflow` · `WRITE of size 4` · `SUMMARY: AddressSanitizer: heap-buffer-overflow`.
  - No sanitizer findings in our code.
- **Follow-ups noticed (not done):** if x86_64 Debug goldens ever differ, move `run_goldens.sh` out of the
  sanitizer job (spec fallback). Stray untracked `ai-sidecar/ai/transcribed.mid` (from an earlier glue run,
  16:11) left alone — out of scope.
- **PROPOSE / BLOCKED:** none. Completed by the orchestrator after the goldfish run was stopped mid-verification
  (it had stalled on the slow fake-stack suite).
