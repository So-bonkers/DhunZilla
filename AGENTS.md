# AGENTS.md — read this before writing anything in this repo

You are a coding agent working on OpenPattern, an open-source AI-native DAW. The human is the
product owner: they approve decisions and review demos; **all code is agent-written**.
Follow these rules exactly. When in doubt, these rules beat your instincts.

## Role & workflow
1. Work ONLY on the assigned work package (`TASKS/WP-XXXX-*.md`). Do not refactor, rename, or
   "improve" anything outside its scope. If something outside scope must change, write a
   `PROPOSE:` note in the handoff report instead.
2. Definition of done (all required before requesting human review):
   - CI green: build (gcc/clang), clang-format where available, unit tests, golden renders, sidecar pytest.
   - Acceptance criteria from the work package checked off **with evidence** (commands + output).
   - `docs/spec/` updated if observable behavior changed; ADR drafted if an architecture decision was made.
   - Demo artifact attached when audio output is affected: before/after WAVs or screenshots.
3. Blocked >30 minutes on the same error? **Stop.** Write `BLOCKED:` in the task file with a
   minimal reproduction. Do not hack around it, disable tests, or mark things "expected to fail".
   Never edit golden/fixture files unless the work package explicitly authorizes it.
4. Handoff report at the bottom of the task file: what changed, how verified, metrics, follow-ups.

## Hard architecture rules
- **Model-driven:** every project mutation is a serializable operation applied to the project
  model (`src/engine/model.*`). UI/scripting are thin layers over it. Never mutate engine state
  from CLI/UI code directly.
- **Real-time thread = dumb executor** (applies once live audio lands): no allocations, locks,
  file I/O or Python calls in any audio callback. All logic off-thread. Established SPSC patterns
  only — never invent lock-free code.
- Module boundaries: `src/engine/` (C++ core), `src/cli/`, `harness/`, `ai-sidecar/` (Python).
  Cross-module contact happens through the interfaces in `docs/spec/` only. A work package
  touching >~5 interface files was mis-scoped — split it.
- The C++ core must stay **GUI-free and dependency-light**: vendored single-header libs in
  `third_party/` with recorded licenses only (currently nlohmann/json, MIT). JUCE integration is
  ADR-gated (see docs/adr/0002) — do not add it casually.

## Determinism (non-negotiable — the harness depends on this)
- Fixed seeds for anything random; no wall-clock time anywhere in engine paths — transport
  position derives from the sample counter only.
- Tests run at 48 kHz float32. Compile with `-ffp-contract=off`; **never** enable fast-math.
- `opd render <project> -o out.wav --seed N` must be bit-exact run-to-run and across our CI arches (x86_64, aarch64).
- Engine math: use libm calls only via `engine/dsp.hpp` wrappers; no vendor intrinsics without an ADR.

## Dependencies & licensing
- Native deps pinned in `third_party/versions.lock`; bumping = ADR-level change.
- AI model weights are NEVER committed. Reference via `ai-sidecar/models/manifest.json`
  (url + sha256 + license tri-state + capabilities). Do not add any model whose license you have
  not verified and recorded. Non-commercial/personal-use weights require `license_tier` set and stay default-off.
- No secrets or credentials in the repo, ever.

## Code conventions
- C++20, compiled locally with g++ 13 (CI: gcc + clang). Warnings-on (`-Wall -Wextra`); fix them.
- Python 3.12 managed by **uv** in `ai-sidecar/`; ruff + mypy --strict; pytest for tests.
- Public APIs get doc comments with a usage example. Tests live next to code, written with the implementation.
- Commit style: `WP-XXXX: imperative summary` — one package per commit/branch series.

## Local reference rig facts (dev environment)
- DGX Spark: **aarch64** Linux, 20 cores, Ubuntu 24.04 kernel-nvidia; GPU via NVML unavailable in
  this container context → CPU inference paths must always work and are tested here. This machine
  IS our linux-arm64 target; regressions found here count as platform bugs.
- ALSA + JACK headers present (live-audio PoCs possible later); no clang locally; `uv` available.
- **Supported platforms: Linux x86_64 + aarch64 only** (ADR-0006). Do not add macOS/Windows
  build paths, CI targets, or platform-specific code to the engine; keep `src/engine/` and the
  CLI portable so a port stays a build job. Ports are deferred, not rejected.

## Style of this project
Boring, explicit code wins. Cleverness is a bug. If you need a trick to pass a test, the design
is wrong — escalate instead.
