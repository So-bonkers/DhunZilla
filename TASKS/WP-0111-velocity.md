# WP-0111 — Note velocity (dynamics)

**Spec:** docs/spec/project-model.md §velocity · **Status: done** · Owner: orchestrator

## Scope
`note.vel` ∈ (0,1] in model/renderer; MIDI import captures note-on velocity; export writes it back.

## Acceptance — evidence
- Parse default/explicit; rejects 0 and >1 (`model.rejects_vel_zero`, `..._above_one`).
- Save omits "vel" at default → legacy canonical bytes unchanged (asserted on serialized text).
- **Legacy bit-exactness:** all six pre-existing goldens still match after the renderer change.
- Gain law: half velocity halves peak to within 1e-6 relative (`vel.half_velocity_halves_peak`).
- MIDI: fixture vel-90 → imported notes exactly 90/127; export→reimport within ±½ step.
- op_tests **128 assertions ALL PASS**; goldens 6 fixtures.

## Design note
Velocity multiplies the mix target's gains (copied per non-default note) instead of touching
voice code — provably bit-safe for defaults, zero cost when unused, and applies uniformly to all
four voice kinds including samplers (where velocity-scaling is the acoustic convention).

## Follow-ups noticed
- Accent (metronome downbeat) and velocity compose multiplicatively — fine; UI should show both.
- Velocity curves / humanization = UI-era feature (record & quantize), not engine.
