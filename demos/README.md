# Ear-check pack — for human review (agents can't hear; you can)

Regenerate anytime (deterministic):

```bash
./harness/make_earcheck.sh                       # files 01–04
OP_TRANSCRIBE_PYTHON=/path/to/onnx-venv/bin/python ./harness/make_earcheck.sh   # + 05/06
```

| File | What to listen for | Verdict we need |
|---|---|---|
| `01_tempo_drop.wav` | Metronome + melody: **tempo drops 120→72 BPM at bar 5**. Melody notes before/after the drop should feel correctly placed, no drift or clicks. | natural? click-free? |
| `02_melody_synth.wav` | The built-in `synth` voice (fundamental + octave partial): acceptably clean for a placeholder timbre? harsh? | timbre note |
| `03_midi_imported.wav` | A hand-built MIDI file imported via `opd midi2opj`: two tracks (melody + bass), timing feels deliberate not sloppy? | timing feel |
| `04_panorama_mix.wav` | Pan/gain stress mix: left/right placement distinct, nothing clipping or unbalanced. | stereo image |
| `05_transcribed.mid` | (generated) the MIDI Basic Pitch heard in 02 — open in any DAW if you want | optional |
| `06_reconstructed_from_audio.wav` | **The money shot**: audio 02 → transcription → import → re-rendered. If 02 and 06 sound like the same tune, the AI loop works with your ears, not just our F1 metric. | same melody? |

Notes for reviewers: placeholder timbres are sine/partial-based by design — judge **structure,
timing, stereo**, not musicality. Any click/drift/wrong-note finding is a bug: file it against
the fixture name and we trace it to the assertion that should have caught it.
