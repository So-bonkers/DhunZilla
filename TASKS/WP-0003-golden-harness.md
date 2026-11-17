---
id: WP-0003
title: Golden-file harness (golden_diff.py + run_goldens.sh) with committed fixtures
phase: 0
status: done
depends_on: [WP-0002]
estimate_agent_hours: 3
human_review: none beyond CI
---

## Handoff report
- **What changed:** harness/golden_diff.py (stdlib-only bitexact/dbfs diff, machine-parsable RESULT line), harness/run_goldens.sh (double-render determinism + golden match per fixture), tests/fixtures/goldens/{demo.opj,demo.wav,kits/kick.wav}.
- **How verified:** `./harness/run_goldens.sh` → "PASS demo: deterministic + golden match / 1 fixtures, 0 failures" exit 0. Golden sha256 prefix 8fafe2f9.
- **Follow-ups noticed:** add a deliberately-corrupt-fixture negative test (harness must FAIL) in WP-0007 CI work; more fixtures (panorama, sampler-stereo, tempo variants).
