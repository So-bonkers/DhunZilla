# WP-0129 — Rename product to **DhunZilla**

**Status:** done — CI green · **Created:** 2026-11-18 · **Owner:** orchestrator
**Decision:** product owner chose "DhunZilla" on 2026-11-18 (see
`docs/legal/trademark-screen-2026-11-18.md`). `dhunzilla` namespace was free at decision time.

## Goal
Replace the provisional "OpenPattern" name on all **user-facing** surfaces with **DhunZilla**
(pron. *dhoon-ZIL-uh*), without touching engine behavior or test fixtures.

## Human steps (blocking, do first)
1. Buy domains: `dhunzilla.com`, `.io`, `.app`, `.dev` (optional: `.in`, `.co.in`, `.music`).
2. Rename the GitHub repo `So-bonkers/OpenPattern` → `DhunZilla` (Settings → Rename). Redirects
   keep old links working; then update the local remote:
   `git remote set-url origin git@github.com:So-bonkers/DhunZilla.git`.
3. Create the GitHub org/handle `dhunzilla` if the repo should live under it (optional).

## Scope (in)
- `README.md`: title, tagline, pronunciation line, links.
- `AGENTS.md`, `ROADMAP.md`: product name in prose and headings.
- `CMakeLists.txt`: `project(openpattern CXX)` → `project(dhunzilla CXX)`.
- `ai-sidecar/pyproject.toml`: project name → `dhunzilla-sidecar` (+ regenerate `uv.lock`,
  verify `uv sync --offline` still resolves).
- CI badge/link URLs, `.github/` metadata, docs index links.
- Add `docs/spec/` note or README line: "Formerly OpenPattern (renamed 2026-11-18)."

## Scope (out)
- **Historical documents keep the old name** — `NIGHTLY_REPORT.md`, `docs/adr/0001` and other
  dated records are not rewritten; add a one-line "(formerly OpenPattern)" note where useful.
- Engine identifiers, file formats (`.opj`/`.opd`), and the `opd` binary stay for now — renaming
  the CLI & Python packages (`opd`, `opsidecar`, `ai_sidecar`) is a separate mechanical WP
  (propose WP-0130) because it touches every harness script and test.
- Golden fixtures, WAV bytes, project-format fields.

## Acceptance criteria
- [x] `grep -ri openpattern` on tracked non-historical files returns only intentional
      "(formerly OpenPattern)" notes and historical docs.
- [x] `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` clean.
- [x] `./build/op_tests` all pass; `./harness/run_goldens.sh` 6/6; `./harness/test_cli_errors.sh` pass.
- [x] `cd ai-sidecar && uv run pytest -q` green; `uv run ruff check .` green.
- [x] CI green on `main` after push (Linux x64 + arm64) — run 44283789875: core linux-x64, core linux-arm64, ai-sidecar all success.
- [x] README renders with the pronunciation and the "formerly OpenPattern" line.

## Handoff report
Append below when done: what changed, evidence (commands/output), anything deferred.

## Handoff report — 2026-11-18

**What changed**
- `README.md`: DhunZilla title, pronunciation line (*dhoon-ZIL-uh*), "Formerly OpenPattern", body copy.
- `AGENTS.md`: agent role line now "DhunZilla (formerly OpenPattern)".
- `ROADMAP.md`: title + name decision points at this WP (renumbered WP-0129; WP-0128 is the .opd fuzzer).
- `CMakeLists.txt`: `project(dhunzilla CXX)`.
- `ai-sidecar/pyproject.toml`: dist name `dhunzilla-sidecar`, header/description; `uv.lock` regenerated
  (`uv lock --offline`; only the project block changed).
- Sidecar strings: FastAPI title, `__init__`/`base`/`wav` docstrings, CLI description.
- `src/cli/main.cpp`: header comment only.
- Historical docs deliberately unchanged: `NIGHTLY_REPORT.md`, `docs/adr/0001`, the trademark report,
  and old `TASKS/` files keep the old name as dated records.

**Evidence (local, aarch64)**
- `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` → clean.
- `./build/op_tests` → **197/197 pass**.
- `./harness/run_goldens.sh` → 6 fixtures, 0 failures (bit-exact + negative control).
- `./harness/test_cli_errors.sh` → 0 failures.
- `cd ai-sidecar && uv run --offline pytest -q` → **88 passed, 2 skipped**.
- `uv run --offline ruff check .` → all checks passed.

**CI**
- `6603053` → https://github.com/So-bonkers/DhunZilla/actions/runs/44283789875 — all three jobs green.

**Deferred / follow-ups**
- `opd`, `opsidecar`, `ai_sidecar` identifiers unchanged → proposed **WP-0130** (mechanical CLI/package rename).
- Human steps: buy `dhunzilla.*` domains; GitHub repo renamed by owner (remote updated to
  `So-bonkers/DhunZilla`).
- Former-URL redirects keep old links alive; CI badge/link URLs work because GitHub redirects.
