# Spec: Project Model (`.opj` v0) & Render CLI

The project file is JSON (v0; SQLite/zip packaging deferred). Field names are stable API.

```jsonc
{
  "format_version": 0,
  "bpm": 120.0,                    // tempo at bar 0; tempo points come later (WP-001x)
  "sample_rate": 48000,            // engine rate for v0 renders
  "seed": 42,                      // master RNG seed (determinism)
  "length_bars": 4,                // render length = bars * beats_per_bar * (sr*60/bpm) samples
  "beats_per_bar": 4,
  "tracks": [
    {
      "name": "Metronome",
      "gain_db": -6.0,             // summed with mixer gain at render time
      "pan": 0.0,                  // -1..1, equal-power panning (wrapper in dsp.hpp)
      "voice": { "kind": "metronome", "accent_every": 4 }   // click: 1000 Hz bar / 800 Hz beat, 30 ms exp decay
    },
    {
      "name": "Bassline",
      "gain_db": 0.0, "pan": 0.0,
      "voice": { "kind": "sine", "freq_hz": 55.0 }          // test voice; synth params grow later
    },
    {
      "name": "Drums",
      "gain_db": -2.0, "pan": 0.0,
      "voice": { "kind": "sampler", "path": "kits/kick.wav" } // nearest-neighbour playback, no resample v0 (48k assets only)
    }
  ],
  "patterns": [
    { "track": 0,                      // index into tracks
      "notes": [                        // times in beats (float), from bar 0
        { "t": 0.0, "dur": 0.25 }, { "t": 1.0, "dur": 0.25 }
      ]}
  ]
}
```

## Render semantics (normative)
1. `total_samples = ceil(length_bars * beats_per_bar * sample_rate * 60 / bpm)` — computed once from the sample counter; **no wall-clock anywhere**.
2. Events scheduled by exact sample position: `sample = round(t_beats * sample_rate * 60 / bpm)`.
3. Voice rendering order = track index ascending; sum per block (block size 512), then apply per-track gain_db → pan → accumulate to stereo.
4. Output clamp: none (float); WAV writer converts float→int16 with saturating round-to-nearest-even.
5. RNG: a single seeded PCG/xoshiro instance per render, created from `seed`; voices needing noise draw from it in track order. Same seed ⇒ bit-identical output on x86_64 and aarch64 (no fast-math; `-ffp-contract=off`; libm only via `engine/dsp.hpp`).
6. Metronome click spec: sine at 1000 Hz (downbeat) / 800 Hz, Hann-ramped 5 ms attack, exponential decay τ=8 ms, hard stop at 30 ms.

## CLI (`opd`)
```
opd render <project.opj> -o <out.wav> [--seed N]   # --seed overrides project seed; deterministic
opd gen-testproject <out.opj>                      # canonical fixture generator (fixed seed)
opd info <file.wav|project.opj>                    # duration/sample-rate/format dump
```
Exit codes: 0 ok; 2 bad usage; 3 file not found; 4 parse error; 5 render error. Errors to stderr, machine-parsable single line `error: <code_name>: detail`.

## WAV writer
Canonical PCM16 stereo, little-endian, 48 kHz (v0), header exactly 44 bytes, no metadata chunks — bit-exactness includes the header.

## Arrangement (clips) — additive to v0 (2026-11-18, WP-0101)

Optional top-level `arrangement`: array of clip objects `{ "pattern": int, "start_bar": double >= 0, "repeats": int in [1..4096] }` (`start_bar` default 0.0, `repeats` default 1). Semantics, normative:

- **When `arrangement` is present it is the sole source of playback**: patterns not referenced by any clip render nothing. When absent, legacy v0 semantics apply: every pattern plays once from beat 0, in declaration order — bit-identical to pre-feature core (committed goldens enforce this).
- Clip placement: for repeat `r` in `[0, repeats)`, notes are placed at absolute beat `start_bar * beats_per_bar + r * extent + note.t`, where `extent = max(note.t + note.dur)` over the pattern (**pattern-relative extent**, in beats). If `extent <= 0` (empty pattern), `repeats` is forced to 1.
- Notes whose start frame lands at/after the project window end are dropped; audio extending past the end is clipped by the buffer (unchanged rule).
- Metronome accent phase is **pattern-relative** (`accent_every` matches against `round(note.t)`, not absolute beat position).
- Render order: clips in declaration order, filtered per track — accumulation order is part of bit-exactness.

Canonical save omits `"arrangement"` when empty, so legacy files round-trip byte-identically.

## Synth voice — additive to v0 (2026-11-18, WP-0103a)

Voice `{"kind": "synth", "attack_ms": 5.0, "release_ms": 30.0}` (both optional, ≥ 0).
Notes may carry `"note": <midi number 0..127>`; absent → the voice's `freq_hz` (sine-style
fallback). Frequency: `f = 440 · 2^((note − 69)/12)` (12-TET, A4=440), computed via the
`dsp::powf_` wrapper only. Waveform: fundamental + phase-locked octave partial at amplitude
0.35, normalized by 1/1.35; envelope is linear attack (`attack_ms`) → full sustain → linear
release (`release_ms` at note end). All math deterministic per ADR-0004 (no RNG, no wall clock).
Canonical save emits `"note"` only when present and synth keys only for synth voices — legacy
project files round-trip byte-identically.

## MIDI import (`opd midi2opj`) — WP-0103b (2026-11-18)

`load_midi()` maps a Standard MIDI File to the project model: format 0/1,
ticks-per-quarter-note division only (SMPTE → actionable error); first tempo meta sets `bpm`
(default 120 if absent); velocity-0 Note-On counts as Note-Off; retriggering an open key closes
the previous note at the new tick; notes left open close at the track's last event tick. One op
track (synth voice) per MIDI track that contains notes, named from the track-name meta when
present. Notes are sorted by `(t_beats, midi_note)` — output is independent of event interleaving.
`length_bars = ceil(max_end_beats / beats_per_bar)`, min 1; `sample_rate` 48000, default seed 42.
Files with zero playable notes are rejected (actionable error). Parser bounds-checks every read;
truncation fuzz over the committed fixture must always fail cleanly, never crash or succeed.

## MIDI export (`opd opj2mid`) — WP-0104 (2026-11-18)

`save_midi()` writes SMF format 1 @ 480 ppq: track 0 carries tempo meta (from project bpm)
and a 4/4 marker; one track per op track with name meta and all its patterns' notes (gathered
by `pattern.track`, not positional). Notes sort by tick, note-offs before note-ons at equal
ticks. Beat→tick is `llround`; zero/negative durations clamp to 1 tick so output is always
well-formed. Notes lacking `midi_note` map via nearest-12-TET of `voice.freq_hz` (table scan,
`dsp::powf_` only). Byte-deterministic for a given Project; round-trip through `load_midi`
preserves track count, bpm, and exact `(t_beats, midi_note)` content for tick-aligned input.

## Tempo map — additive to v0 (2026-11-18, WP-0105)

`"tempo_map": [{"bar": 0.0, "bpm": 120.0}, ...]`: bar-anchored piecewise-constant tempo.
Empty/absent = constant `bpm` (legacy path — bit-exact, goldens prove it). Non-empty: strictly
increasing bars ≥ 0, bpm ∈ (0, 1000], **first point must be at bar 0**, and it governs from
onward (`bpm` field then only matters when the map is absent). Bars keep `beats_per_bar` beats.
Beat→sample conversion is piecewise: each full segment contributes `llround(dur_beats · spb)`
samples accumulated exactly; note start = segment base + `llround(local_beats · spb)`. A single
point `{0, bpm}` must render bit-identically to no map (tested). **Duration simplification:** a
note's length uses the local tempo at its start — notes crossing a change do not stretch inside
(spec choice; revisit with audio-warped sampler if musical need appears). Total frames = full
segments + `ceil` of final partial. Canonical save omits an empty map. MIDI import/export ignore
tempo maps in v0 (first tempo → constant); follow-up noted.
Note-event data bytes ≥ 0x80 (illegal per SMF) are ignored — the event is skipped and parsing
continues; real-world files never fail import on quirk bytes, and model invariants (pitch range,
dur > 0) hold under mutation fuzzing (500 seeded mutants, deterministic).

## Velocity — additive to v0 (2026-11-18, WP-0111)

Notes accept `"vel": <float in (0, 1]>` (default 1.0; anything else rejected). Semantics:
per-note gain multiplier applied **on top of** the track gain, uniformly across all voice kinds
(metronome ticks, sine/synth oscillators, sampler hits). Implementation contract: voices never
see velocity — the renderer copies the mix target with `gain ×= vel` only when vel ≠ 1.0, so a
default-velocity render is bit-identical to pre-WP-0111 (goldens prove it; IEEE guarantees
`x · 1.0 ≡ x`). Canonical save omits `"vel"` when exactly 1.0 → legacy files round-trip
byte-identically. MIDI: import maps note-on velocity → `vel/127`; export writes
`clamp(round(vel·127), 1, 127)`; a MIDI round-trip preserves velocity within ±½ MIDI step.

## MIDI merge — additive to v0 (2026-11-18, WP-0114)

`opd import-midi <proj.opj> <in.mid> [-o out.opj]` appends each note-bearing MIDI track as a new
synth-voice op track at the end of an **existing** project (the hum→MIDI result lands alongside
your drums, not in a separate file). Semantics:
- **Grid mapping:** MIDI beats map 1:1 onto project beats — musical position is preserved; a tempo
  difference does *not* rescale note positions (documented v0 choice; a `--rescale` mode is a
  possible future flag, not implied).
- **Track indices** are assigned sequentially from the current track count; pattern `.track` fields
  stay consistent. On any failure the target project is left untouched (merge builds in a temp first).
- **Length:** extends `length_bars` to fit imported content; never shrinks it.
- **Arrangement interaction:** if the host has an arrangement, imported tracks are silent until a
  clip references them — the CLI prints that note rather than surprising the user with quiet output.
This is a model-driven mutation (load → engine op → save); no direct state mutation from the CLI.

## Tempo map ⇄ MIDI (2026-11-18, WP-0115)

**Export:** `save_midi` writes one FF51 meta per `tempo_map` point at tick `bar·beats_per_bar·480`
(monotonic-clamped). An empty map exports as the single `{bar 0, bpm}` point — byte-identical to
pre-WP-0115 output (asserted). **Import:** all FF51 events are collected across tracks (format-1
ticks share one timeline), sorted by tick; consecutive same-tempo/non-increasing entries collapse.
With ≥2 distinct points the project gets a `tempo_map` (bar-0 anchored: an implicit `{0, first}`
is prepended if the earliest event is later); **single-tempo files keep the legacy empty-map path**
so existing imported fixtures are unchanged. Tempos outside (0,1000] bpm fail actionable — matching
the model's validation ceiling; length comes from notes only (a late tempo point never extends it).
Round-trip `opj → mid → opj` preserves bar positions exactly and BPM within µs quantization (<1e-3).
