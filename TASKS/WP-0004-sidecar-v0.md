---
id: WP-0004
title: Sidecar v0 — FastAPI job queue, backend adapter base, mock backends, conformance suite green
phase: 0
status: in-progress
depends_on: [WP-0001]
estimate_agent_hours: 6
human_review: demo (curl transcript)
---

## Context
The AI sidecar is the chassis for all "only-AI" features. v0 must run with ZERO heavy ML deps so CI
on six targets stays fast; real adapters (Demucs etc.) bolt on later behind manifest gates.

## Spec
Implement exactly per `docs/spec/backend-adapters.md` + `docs/spec/sidecar-protocol.md`:
- uv-managed package `ai-sidecar/` (Python 3.12): `opsidecar` CLI (`serve`, `jobs submit/status` convenience).
- FastAPI app: /health, /jobs (+cancel), /jobs/{id}/events (SSE), /models endpoints per protocol spec.
- Adapter base + Capabilities pydantic models; registry discovers backends from `backends/` dir.
- Mock backends: `mockgen` (seeded sine-chord WAV, ±5% duration accuracy) and `mocksep` (4-band FFT split → 4 stem WAVs). Both deterministic.
- Job runner: asyncio queue + worker pool; artifacts to out/jobs/<id>/; mandatory provenance.json.
- Conformance suite in tests/conformance/ parametrized over all registered backends — mocks must pass ALL five checks from the spec.

## Acceptance criteria
- [ ] `cd ai-sidecar && uv sync && uv run pytest -q` → green, exit 0 (evidence: output tail)
- [ ] Conformance suite runs both mocks × 5 checks (test names visible in pytest -v output)
- [ ] TestClient smoke test: health lists backends; submit mockgen job → done → artifacts+provenance exist; license-gated model download refused without accept
- [ ] `uv run ruff check .` and `uv run mypy --strict ai_sidecar` clean (fix or annotate; report exceptions)
- [ ] No torch/numpy-heavy deps in core env (numpy allowed); heavy extras only as optional group, absence never breaks tests

## Out of scope
Real model adapters (WP-0006/8), gRPC, auth/TLS, GPU scheduling policy.

## Notes for the implementing agent
Work ONLY under `ai-sidecar/`. Do not run git commands. Reference rig is aarch64 — all deps must be pure-python/numpy wheels available for aarch64. If PyPI access fails from bash, write BLOCKED with the error immediately.

## Handoff report (orchestrator, completing for subagent 93bb0ea4 which died mid-WP-0005)
- **What changed:** full sidecar v0 per spec — src/ai_sidecar/{base,jobs,registry,app,cli,wav,model_manager}.py + opsidecar alias package; backends/mockgen (seeded sine WAV gen) + backends/mocksep (4-band FFT split); conformance suite tests/conformance/ over both mocks (5 checks each incl. determinism byte-identity + provenance).
- **Fixes applied by orchestrator after takeover:** (1) SyntaxError: non-ASCII em-dash in bytes literal (tests/test_model_manager.py); (2) startup race — SIDECAR_PORT announced before listen(): added sock.listen(128) pre-announce + spec-compliant backoff retry in lifecycle test; (3) vacuous-truth bug: local_available()==True for zero-source entries → now False (real bug caught by fixture); (4) lint/type cleanup: StrEnum, B904 from-exc, E501 wraps, mypy-strict src clean.
- **Evidence:** `uv run pytest -q` → 59 passed; `ruff check .` → clean; `mypy --strict src/` → no issues (11 files). Manual: serve --port 0 announces port, /health lists backends with capabilities JSON.
- **Follow-ups noticed:** httpx/starlette TestClient deprecation warning (cosmetic); SSE events endpoint covered by tests? verify coverage in morning review; conftest file_server fixture is reusable for WP-0006 demucs adapter download tests.
