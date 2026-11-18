---
id: WP-0005
title: Model manager — manifest schema, sha256-pinned downloader, license tri-state gates
phase: 0
status: in-progress
depends_on: [WP-0004]
estimate_agent_hours: 3
human_review: none beyond CI + spec diff
---

## Context
Weights never ship in git; the manifest is the sole gate (docs/spec/model-manifest.md). This is also
the pre-wiring point for unreleased models (`status: awaiting-weights` stubs, e.g. next ACE-Step).

## Spec
Inside `ai-sidecar/`: `model_manager.py` + `models/manifest.json` seeded with real entries verified
2026-11-18 (htdemucs [personal-use-ok], acestep-v15 [verify current weights license — code Apache-2.0,
weights repo has an open clarification request → tier personal-use-ok until confirmed], yue2-3b
[personal-use-ok per MODEL_LICENSE], basic-pitch-onnx [permissive if ONNX export is Apache], plus one
`awaiting-weights` stub for the announced-but-unreleased ACE successor). Downloader: httpx, range-resume,
sha256 verify → quarantine on mismatch, cache_dir per entry. License gate: download refused unless
tier==permissive OR explicit accept flag passed through API (spec/sidecar-protocol.md /models/{id}/download).

## Acceptance criteria
- [ ] manifest validates against schema (pydantic), all entries have license_tier + verified_on + sha256-pinned sources (use placeholder hashes for entries whose real hashes can't be fetched offline — mark `hash_verified: false` honestly)
- [ ] pytest: download+verify happy path via local file:// or in-process test server; sha256 mismatch → quarantine; personal-use-ok refused without accept; permissive allowed; awaiting-weights refused with actionable error
- [ ] /models endpoint surfaces tier + availability + status correctly

## Out of scope
Actual multi-GB weight downloads in CI (test with tiny synthetic files only).

## Notes for the implementing agent
Same ownership rules as WP-0004: `ai-sidecar/` only, no git. Keep honest — placeholder hashes must be marked, never fabricated as real.

## Handoff report (orchestrator, completing for subagent 93bb0ea4)
- **What changed:** model_manager.py (pydantic schema, httpx downloader w/ range-resume + sha256 verify → quarantine on mismatch, license tri-state gate incl. awaiting-weights refusal), models/manifest.json with 5 entries: htdemucs/acestep-v15/yue2-3b (personal-use-ok), basic-pitch-onnx (permissive), acestep-next (awaiting-weights stub w/ unverified claims marked).
- **Evidence:** included in the 59-pass suite; manifest honesty verified programmatically: placeholder hashes carry hash_verified:false. Vacuous-availability bug fixed + regression-tested.
