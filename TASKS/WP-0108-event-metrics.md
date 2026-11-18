# WP-0108 — Event-level round-trip metric (onset tolerance) + stem HTTP demo

**Spec:** n/a (harness) · **Status: done** · Owner: orchestrator
Absorbs WP-0107 (stem_http_demo.sh — see ROADMAP row).

## Scope
Upgrade harness/roundtrip_eval.sh from pitch-multiset F1 to also compute event F1
(pitch exact, onset within TOL_S default 0.08s, greedy nearest matching); gate both ≥0.8.

## Acceptance — evidence
- 3 runs: PITCH F1=1.00, EVENT F1=1.00 every run; ONNX inference byte-stable across runs here
  (detected onsets identical), max onset drift 25ms vs 80ms tolerance → default gate safe.
- Gates env-tunable (F1_THRESHOLD / EVENT_F1_THRESHOLD / TOL_S); README snippet updated to real output.

## Follow-ups noticed
- Declared `deterministic=False` for basicpitch stays honest (thread scheduling can vary on other
  hardware); our metric gate doesn't depend on determinism, only the margin analysis did.
- Next corpus target: real humming samples once available from the product owner (ear-check list).
