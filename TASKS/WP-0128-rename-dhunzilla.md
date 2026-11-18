# WP-0128 — Rename product to **DhunZilla**

**Status:** ready · **Created:** 2026-11-18 · **Owner:** orchestrator
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
  (propose WP-0129) because it touches every harness script and test.
- Golden fixtures, WAV bytes, project-format fields.

## Acceptance criteria
- [ ] `grep -ri openpattern` on tracked non-historical files returns only intentional
      "(formerly OpenPattern)" notes and historical docs.
- [ ] `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j` clean.
- [ ] `./build/op_tests` all pass; `./harness/run_goldens.sh` 6/6; `./harness/test_cli_errors.sh` pass.
- [ ] `cd ai-sidecar && uv run pytest -q` green; `uv run ruff check .` green.
- [ ] CI green on `main` after push (Linux x64 + arm64).
- [ ] README renders with the pronunciation and the "formerly OpenPattern" line.

## Handoff report
Append below when done: what changed, evidence (commands/output), anything deferred.
