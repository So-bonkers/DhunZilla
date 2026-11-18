# WP-0122 — JSON model parser fuzzing

**Status: done** · Owner: orchestrator · Spec: docs/spec/project-model.md (load failure contract unchanged: bool + err, never a throw)

## Scope
Seed corpus (all field families) → truncation sweep + 500-trial seeded mutation + **type-confusion**
class (every top-level key × wrong-typed value). Fix whatever escapes. No new deps.

## Acceptance — evidence
- BEFORE fix: targeted probe showed **22 escaping `type_error.302` paths** (bpm:[], tracks:"str", …)
  — an uncaught exception aborts any process loading a hostile .opj/.opd. Random-byte fuzz alone
  found ZERO of these; the deliberate type-swap class was required. Both classes now permanent tests.
- AFTER fix (`load_project` try-covers parse+validate, parses into tmp → atomic out):
  probe re-run **0 escapes**; `fz.type_confusion_never_escapes` (50 cases) + truncation + mutation
  ALL PASS; op_tests **182 assertions**; ASAN+UBSAN full sweep clean.
- Goldens untouched (6/6 bit-exact); CLI error harness OK; no consumer depended on the old
  "json parse:" prefix (grepped).

## Handoff report
**What changed:** model.cpp load_project (~10 lines: catch covers parse_project too; tmp+move makes
out atomic even mid-throw), test_main.cpp (+fz suite, ~120 lines), ROADMAP.
**Method lesson:** random mutation and targeted type confusion are complementary — record it:
"shape fuzzers find crash-shaped bugs; schema-aware fuzzers find contract-shaped bugs."
Also re-learned the hard way: LD_PRELOAD on a g++ command line poisons the compile (leak banners
from compiler subprocesses) and stale binaries then mask failures — never co-chain build+preload runs.
**PROPOSE (ADR-0006 follow-up, out of this WP's scope):** remove the two pre-existing `#ifdef _WIN32`
drive-letter checks in model.cpp/bundle.cpp — dead code under "Linux x86_64 + aarch64 only"; keep the
portable '/'-based validation. One tiny cleanup WP when convenient.
**Follow-ups noticed:** corpus could later include hand-written hostile .opj fixtures for cross-arch
CI parity (same failure messages on gcc vs clang — needs the GitHub remote, human-blocked).
