# WP-0126 — Sidecar WAV reader robustness (mirrors WP-0125)

**Status: done** · Owner: orchestrator · Follows PROPOSE in TASKS/WP-0125-wav-fuzz.md

## Scope
`ai_sidecar.wav.read_wav` gets the same field-domain validation as the C++ reader, plus crafted +
truncation + 500-trial mutation tests (tests/test_wav_robustness.py). No API change: clean ValueError.

## Findings — evidence
- Pre-fix: **bits=0 → ZeroDivisionError** (`len(data) % block_align` with block_align=0) — would surface
  as an unhandled crash in the job runner instead of a failed-with-reason job.
- Also closed: channels>2 (outside contract; reshape accepted it), sample_rate > 1e6 (C++ parity).
- New tests: 8 crafted header cases + 7 truncations + 500 mutations — all must raise ValueError or
  return finite samples. **Sidecar suite 88 passed, 2 skipped**; ruff + mypy --strict clean.
- Test bug caught by the suite itself: first crafted case patched tag→3 on an already-float seed
  (no-op → DID NOT RAISE); replaced with the real float-tag+16-bit confusion pair. Parametrized
  tests fail loudly — exactly why we write them.

## Handoff report
**What changed:** wav.py +7 validation lines; new test file (~90 lines); ROADMAP.
**Contract now symmetric across languages:** PCM16 or float32, mono/stereo, sr ∈ (0, 1e6], invalid →
clean error at the boundary in both readers.
**No further fuzz gaps known** for untrusted file inputs; remaining ideas are generate-backend eval
(needs model/license decision) and human-blocked CI/ear-check items.
