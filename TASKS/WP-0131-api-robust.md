# WP-0131 — Sidecar API robustness

**Status: done** · Owner: orchestrator · Completes the untrusted-input matrix at the HTTP boundary.

## Scope
tests/test_api_robustness.py: malformed JSON bodies (truncated/garbage/array/string), missing &
unknown backend/task fields, params type confusion, required-field enforcement, over-cap durations
(1e9 s → must not allocate), bad filesystem paths through transcribe (missing/dir/non-WAV), and
lifecycle edges (unknown job 404s, terminal-state cancel, double fetch).

## Results — evidence
- **Zero product gaps found** — Pydantic-at-the-boundary + backend-side caps (mockgen max_duration_s)
  already handled every class; all 14 new cases pass against unmodified code. Suite: **102 passed,
  2 skipped**; ruff + mypy --strict clean. A green hardening round is still progress: the contract is
  now pinned by tests instead of folklore.
- Contract fact learned the test-first way: **generate params REQUIRE `seed`** (missing seed → job
  fails with pydantic "Field required") — my first draft omitted it and the suite said so immediately.

## Handoff report
**What changed:** one test file + ROADMAP; no product code touched (correct outcome for this WP).
**Untrusted-input matrix now complete:** MIDI / JSON model / WAV ×2 languages / .opd structure / HTTP API.
Remaining known human-gated items: real-voice eval corpus, sine-pitch semantics decision, ear-checks.
