# WP-0127 — Transcription fidelity corpus

**Status: done** · Owner: orchestrator · Extends plan §9 eval from 1 case to a measured corpus.

## Scope
tests/fixtures/eval/{scale_low,scale_high,clicky,legato}.opj (synth voice; register extremes,
fast-attack staccato, exact-legato) + harness/score_notes.py (scorer extracted from
roundtrip_eval.sh's heredoc — one implementation, now shared) + corpus loop in roundtrip_eval.sh
with per-case gates + import/render product-loop smoke. CI-safe skip 3 without OP_TRANSCRIBE_PYTHON.

## Results — evidence (real Basic Pitch ONNX, ort 1.32, this machine)
    pitched:     notes=6  detected=6  pitch_f1=1.000 event_f1=1.000 PASS
    clicky:      notes=12 detected=12 pitch_f1=1.000 event_f1=1.000 PASS
    legato:      notes=12 detected=12 pitch_f1=1.000 event_f1=1.000 PASS
    scale_high:  notes=12 detected=13 pitch_f1=0.960 event_f1=0.960 PASS   (one extra detection)
    scale_low:   notes=12 detected=12 pitch_f1=1.000 event_f1=1.000 PASS
    roundtrip_eval: 5 cases, 0 failures
Mean event F1 ≈ 0.992 at ±80ms onset tolerance; gates unchanged (0.8). The high-register extra note
is the only imperfection — plausible octave/split artifact at B5; kept in corpus as an honest signal,
gate has margin.

## Handoff report
**What changed:** new scorer module + fixtures + rewritten eval loop; no engine/sidecar code touched.
**Scope honesty (for product claims):** these numbers measure synthetic monophonic synth material —
clean by construction. Real humming adds vibrato/timing/portamento; expect materially lower F1 and
treat the corpus as a *regression guard for the pipeline*, not a claim about human input quality.
A real-voice corpus needs recording/licensing decisions → human decision territory.
**Engine note discovered while building:** sine voice is constant-frequency (v.freq_hz); only synth
derives pitch per note from midi_note. If users will expect sine tracks to play melodies, that's a
one-line engine change + golden re-blessing discussion — PROPOSE for a future WP decision.
