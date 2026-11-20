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
opd apply <proj.opj> <ops.opjl> [-o out.opj]       # apply JSON-Lines ops journal, all-or-nothing (docs/spec/operations.md)
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

## Clip gain — additive to arrangement (2026-11-18, WP-0120)

Clips accept `"gain_db": <float in [-96, 24]>` (default 0.0; rejected outside range or non-finite).
Per-clip level applied on top of track gain, uniform across all repeats of the clip; composes
multiplicatively with per-note velocity. Implementation mirrors velocity: one mix-target copy per
non-unity clip, none at unity → `gain_db` absent/0 renders **bit-identical** to pre-WP-0120
(goldens prove it; `pow(10, ±0) ≡ 1` exactly). Canonical save omits the field at exactly 0.0.
Use cases: intro builds, ducking an loop under a stem, dropping a section's energy — no note edits.

## MIDI export flattens arrangement (2026-11-18, WP-0130)

`opd opj2mid` now expands the arrangement exactly like the renderer: clips placed at
`start_bar·beats_per_bar + r·pattern_extent`, repeats honored. MIDI has no clip-level gain, so a
clip's `gain_db` is **baked into exported note velocities** (clamped to 1..127). Projects without an
arrangement export byte-identically to pre-WP-0130 output (proved against the previous binary).

## Mixer: mute / solo / master gain — additive to v0 (WP-0136)

Track fields: `"mute": bool` (default false), `"solo": bool` (default false). Project field:
`"master_gain_db": <float in [-96, 24]>` (default 0.0; rejected outside range or non-finite).
Canonical save omits each field at its default → legacy projects save byte-identically.

Audibility: let `any_solo` = some track has `solo: true`. Track t is **audible** iff
`!t.mute && (!any_solo || t.solo)`. (Mute wins over solo.)

Render: inaudible tracks are **skipped entirely** (not rendered at gain 0), so a project with no
mute/solo fields renders bit-identically to before. `master_gain_db` multiplies every output sample
after all tracks are summed; at exactly 0.0 the multiply is skipped (bit-identical).

Stems (`--stems-dir`): one stem per track as before; an inaudible track's stem is all-zero samples of
the full render length (file still written), and master gain is applied to every stem, so the
existing invariant holds: sum of stems ≈ mix (float-associativity only).

MIDI export (`opj2mid`) ignores mute/solo/master: it exports data, not the mix.

Validation rules closed alongside (WP-0136): every track's `gain_db` must be finite and within
[-96, 24], every `pan` finite and within [-1, 1]; the parser rejects violations with exactly
`track.gain_db out of range [-96, 24]` / `track.pan out of range [-1, 1]`, so the same checks gate
`set_track_gain` / `set_track_pan` through their save→parse validation. Non-boolean `mute`/`solo`
values are rejected with `track.mute must be a boolean` / `track.solo must be a boolean`;
`master_gain_db` out of range or non-finite is rejected with `master_gain_db out of range [-96, 24]`.

## Sampler resampling — additive (WP-0138)
A sampler asset whose sample rate differs from the project `sample_rate` is converted once, when loaded,
to the project rate; playback then proceeds exactly as for a native-rate asset. Assets already at the
project rate take the existing path untouched (bit-identical to before).

Converter (fixed, so renders are bit-exact on every CI arch):
- Band-limited interpolation with a windowed sinc. Half-width N = 16 zero crossings of the *input*
  sinc on each side; Blackman window over that span; cutoff fc = 0.97 × min(1, out_rate / in_rate)
  (fraction of input Nyquist) — the kernel is stretched by 1/min(1, out/in) when downsampling.
- Output sample k sits at input position t = k × in_rate / out_rate, computed as
  (k × in_rate) / out_rate with 64-bit integers for the integer part and a double for the fraction
  (no cumulative float drift). Output length = ceil(in_frames × out_rate / in_rate).
- Taps outside the input are zero. Each output sample = Σ x[i]·h(t − i) normalized by Σ h(t − i) over
  the taps actually used (keeps DC gain exactly 1 near the edges).
- sin/cos only via `engine/dsp.hpp` wrappers; double accumulation; result stored as float.
- Supported input rates: 8000–192000 Hz. Outside that range → render error
  `sampler: unsupported sample rate <n>` (exit 5).

## Render a bar range — additive (WP-0145)
`opd render <proj.opj> -o <out.wav> [--seed N] [--stems-dir DIR] [--from-bar A] [--to-bar B]`

**Bar numbering is 0-based**, exactly like `start_bar` and `tempo_map[].bar`: bar 0 is the first bar,
and bar `length_bars` is the end of the song. A and B are real numbers; **fractional bars are allowed**
(`--from-bar 1.5` is the middle of the second bar). Defaults: A = 0, B = `length_bars`.

**Bar → sample map `S(bar)`** (engine function `sample_at_bar`, shared by CLI and tests):
- if `bar >= length_bars`: `S = total_samples_for(project)` (the full render length, spec §1);
- otherwise `S = min(at(bar · beats_per_bar), total_samples_for(project))`, where `at(beats)` is the
  same beat→sample conversion the renderer uses to place note starts: constant tempo →
  `llround(beats · sample_rate · 60 / bpm)`; with a tempo map → the piecewise rule of the Tempo map
  section (segment base samples accumulated with `llround`, plus `llround(local_beats · spb)` inside
  the segment). `S` is therefore the exact sample at which an event at that bar would start.

**Output** is exactly samples `[S(A), S(B))` of the full render (same project, same seed): the output
WAV has `S(B) − S(A)` frames and its data chunk is byte-for-byte equal to bytes
`[4·S(A), 4·S(B))` of the full render's data chunk (PCM16 stereo = 4 bytes per frame); header is the
canonical 44-byte header for that frame count. Nothing is re-timed, re-normalised, faded or padded:
notes that began before A and are still sounding at A are heard exactly as in the full render, and the
render of a range never differs from the slice. (Implementations may render fully and slice — the
reference does — or render efficiently, as long as the bytes are identical.) With `--stems-dir`, the
same `[S(A), S(B))` slice applies to the mix and to every stem file; each stem file is byte-equal to
the slice of the corresponding full stem.

**Validation** (before any file is written; on failure nothing is created). All errors are
`error: usage: <detail>` on stderr, exit 2:
- value not a finite number (whole argument must parse; `abc`, `1x`, empty, `nan`, `inf`) →
  `bad --from-bar value '<arg>'` / `bad --to-bar value '<arg>'`;
- `A < 0` → `--from-bar must be >= 0`;
- `A >= B` (numeric) → `--from-bar must be less than --to-bar`;
- option given without a value → the existing `unknown arg '<arg>'` message;
- (after the project is loaded) `B > length_bars` → `--to-bar <B as given> exceeds length_bars`;
  `A >= length_bars` → `--from-bar <A as given> is not before the end of the song`;
- `S(B) <= S(A)` (range shorter than one sample) → `bar range is empty`.
Project-file errors keep their existing codes (3 not found, 4 parse, 5 render) and take precedence in
that order of discovery: option syntax is checked first, then the project is loaded, then bounds.
## Click track render — `opd click` (WP-0148)
`opd click <project.opj> -o <out.wav>` renders a metronome click track for the whole project, ignoring
tracks, patterns, arrangement, mute/solo, gain and master gain (only timing fields matter: `sample_rate`,
`bpm`/`tempo_map`, `beats_per_bar`, `length_bars`). Engine API (`engine/render.hpp`):
`std::vector<long long> beat_onsets(const Project&)` and
`bool render_click(const Project&, wav::StereoBuffer&, std::string& err)`.

- **Output length** = `total_samples_for(project)` frames, stereo, both channels bit-identical, all
  samples outside click bursts exactly 0.0. WAV via the canonical writer (same as `opd render`).
- **Onsets:** beat `b` = 0, 1, 2, ... (integer beats from the project start; absolute, not per bar) starts
  at sample `at(b)`, the renderer's beat→sample mapping (§Render semantics 2 and §Tempo map):
  constant tempo `llround(b · sr·60/bpm)`; with a tempo map `segment_base_sample + llround((b −
  segment_start_beat) · segment_spb)` (`spb = sr·60/bpm`). Beats are emitted while `at(b) < total`.
- **Accent:** beat `b` is a downbeat iff `b % beats_per_bar == 0`.
- **Burst:** `L = sr·30/1000` (integer division) samples, truncated at the end of the buffer. For
  `n = 0 .. L−1`: `x[n] = 0.5 · cos(2π · f · n / sr) · exp(−n / (sr/200.0))`, `f = 1500` Hz downbeat,
  `1000` Hz otherwise (phase evaluated in double via `dsp::cosd_`, envelope via `dsp::expf_`). `x[0] =
  0.5` exactly, so the onset sample is the first non-zero sample of the burst. Bursts never overlap
  (bpm ≤ 1000).
- Errors: same as `opd render` (exit 2 usage, 3 not found, 4 parse, 5 render error incl.
  `render_error: zero-length render` and the 600 s cap).
### Polyphase tap table — additive (WP-0150)
The converter evaluates taps from a per-call polyphase table instead of recomputing sin/cos per output
sample. Output is bit-identical to the direct method above for every rate pair; the public API is unchanged.
- rem = (k × in_rate) mod out_rate is a multiple of g = gcd(in_rate, out_rate), so there are only
  out_rate / g distinct fractions. Table entry (phase, m) holds the tap for input offset m = ip − i, built
  lazily with the same expressions and operation order as the direct method.
- Window bounds (lo/hi), clipping at the input edges, the normalising sum (summed in ascending i) and the
  accumulation order are unchanged.
- Memory bound: the table holds phases × (2·(ceil(half_w)+2)+1) doubles, at most 2^20 doubles (8 MiB).
  Above that the direct method is used instead (same bits).


## Loop region — additive (WP-0155)
A project may carry one optional transport **loop region**, used by live playback (a later work package) to
repeat a span of the song. Offline rendering is **unaffected**: `opd render` without `--loop` always renders
the whole song, and `render_project` / `render_stems` / MIDI export / `.opd` packing ignore the field.

```jsonc
"loop": { "start_bar": 1.0, "end_bar": 3.0 }   // optional; absent = no loop region
```
- Bars are **0-based** like `clip.start_bar` and `tempo_map[].bar`; **fractional bars are allowed**
  (`"start_bar": 0.5`). `end_bar` is exclusive: the region is `[start_bar, end_bar)`.
- Model: `struct LoopRegion { double start_bar = 0.0; double end_bar = 0.0; };` and
  `std::optional<LoopRegion> loop;` on `op::Project` (`engine/model.hpp`, empty = no loop).
- **Canonical save** writes the key only when set, as an object with keys `end_bar`, `start_bar`. (Canonical
  JSON dumps all object keys in alphabetical order, so `loop` lands between `length_bars` and `patterns`,
  and inside it `end_bar` precedes `start_bar`; no key-order code is needed.) When absent the key is omitted, so every project without a
  loop saves byte-for-byte as before (all goldens unchanged).
- **Validation** at parse (`load_project`), checked in this order, first failure wins; each is the whole
  error string:
  1. `loop` is not a JSON object → `loop must be an object`
  2. `start_bar` or `end_bar` missing or not a JSON number → `loop.start_bar and loop.end_bar must be numbers`
  3. `!(start_bar >= 0)` or start not finite → `loop.start_bar must be a finite number >= 0`
  4. end not finite → `loop.end_bar must be a finite number`
  5. `end_bar <= start_bar` → `loop.end_bar must be greater than loop.start_bar`
  6. `end_bar > length_bars` → `loop.end_bar must be <= length_bars`
  7. any other key inside `loop` → `loop has unknown key '<key>'`
  So `0 <= start_bar < end_bar <= length_bars`, all finite. Because ops validate through the same parse,
  `set_length_bars` to a value below `loop.end_bar` is rejected with
  `set_length_bars: loop.end_bar must be <= length_bars`.
- `Document` (`src/doc/document.hpp`) needs no change: `doc.project().loop` is the read accessor.

### CLI additions (WP-0155)
- `opd info <file.opj>` prints one extra line, only when a loop is set, directly after the `project:` line
  (and after `master_gain_db=` when that line is present, before the track rows):
  `loop: <A>-<B>` where A and B are `start_bar` and `end_bar` printed with C `printf("%g")`
  (`loop: 1-3`, `loop: 0.5-2.5`). Nothing is printed when absent.
- `opd render <proj.opj> -o <out.wav> --loop [other options]` renders exactly the loop region. **Defined as
  identical output** to `--from-bar <start_bar> --to-bar <end_bar>` with the loop's values: same WAV bytes
  (and, with `--stems-dir`, the same stem files). Rules, all `error: usage: <detail>`, exit 2, nothing written:
  - `--loop` combined with `--from-bar` or `--to-bar` (either order) → `--loop cannot be combined with --from-bar/--to-bar`
    (checked with the option syntax, before the project is loaded);
  - project has no loop (checked after load) → `project has no loop region`.
  `--loop` takes no value. Project-file errors keep their codes (3 / 4 / 5) and precedence.
## Sampler trim — additive (WP-0156)
A sampler voice may play a slice of its asset: `{"kind": "sampler", "path": "...", "start_s": 0.0125, "end_s": 0.0375}`.
Both keys are optional and valid **only on sampler voices** (on any other kind → parse error
`voice.start_s/end_s are only valid for sampler voices`).

**Units and reference frame (normative).** `start_s` / `end_s` are seconds on the asset's own timeline,
converted to frame indices of the **project-rate buffer, i.e. after resampling (WP-0138)** — the converter
runs first, unchanged and on the whole asset, and the slice is cut from its output. For a native-rate asset
the buffer is the asset itself, so index `k` is source frame `k`. Rounding rule, exactly:
`i0 = llround(start_s × sample_rate)` and, when `end_s` is present, `i1 = llround(end_s × sample_rate)`
(`sample_rate` = project rate as `double`, product in `double`, `llround` = round half away from zero).
The slice is frames `[i0, i1)` of that buffer; `i1` is clamped to the buffer length `avail` (`i1 = min(i1, avail)`);
with no `end_s`, `i1 = avail`. Examples at 48 kHz: `start_s 0.0125` → `i0 = 600`; `end_s 0.0375` → `i1 = 1800`.

**Defaults.** `start_s` absent or `0.0` and `end_s` absent: `i0 = 0`, `i1 = avail` and the render is
**bit-identical** to pre-WP-0156 output (committed goldens prove it). Canonical save omits `start_s` when it
is exactly `0.0` and omits `end_s` when absent; legacy files round-trip byte-identically.

**Validation** (project parse; the same code path serves `load_project` and operations; checked in this order,
first failure wins, only when either key is present on a sampler voice):
- `start_s` present but not a JSON number, not finite, or `< 0` → `voice.start_s must be a finite number >= 0`;
- `end_s` present but not a JSON number, not finite, or `<= 0` (this includes JSON `null`) →
  `voice.end_s must be a finite number > 0`;
- both effectively present (`start_s` defaults to 0): `end_s <= start_s` → `voice.end_s must be greater than voice.start_s`.
An `end_s` beyond the asset's length is **not** an error at parse time (the asset is not opened by the parser)
and **clamps** at render time as above. At render time, after the asset is loaded (and resampled),
if `i1 <= i0` — start at or beyond the end of the asset, or a slice shorter than rounds to one frame — the
render fails: `sampler: trim slice is empty` (exit 5, same code as other sampler render errors).

**Playback semantics are unchanged, applied to the slice.** A note plays the slice from its first frame:
output frame `n` (relative to the note start) is slice frame `i0 + n`; the note sounds for
`len = min(i1 − i0, llround(note.dur × spb))` frames — a note shorter than the slice gates it (cut with no fade,
as today), a longer note plays the slice once and then is silent (one-shot, no looping). Velocity, pan, gain,
clips and the tempo map apply exactly as for an untrimmed sampler.

## Sampler trim — operation and diff (WP-0156)
See `docs/spec/operations.md` (`set_sampler_trim`). `opd diff` is unchanged: a trim difference is a
`tracks[i].voice` difference and is reported as `unsupported difference: tracks[i].voice` (as any voice change).
## AIFF input — additive (WP-0158)
Sampler assets and `opd info` accept AIFF in addition to WAV. The format is chosen by the file's first
bytes, not its extension: `FORM` → AIFF reader, anything else → the WAV reader (behaviour and error
strings unchanged). Supported: AIFF and AIFF-C (`NONE`, `sowt`), 16- or 24-bit integer PCM, mono or
stereo, any sample rate 1–1000000 Hz that is an exact integer in the 80-bit extended field (the sampler
still accepts 8000–192000 Hz and resamples as for WAV). 16-bit samples convert with the same expression as
WAV, so an AIFF and a WAV with the same samples render byte-identically. Strict validation: error texts
`file too small for AIFF`, `not a FORM/AIFF file`, `COMM chunk too short`, `SSND chunk too short`,
`truncated sound data`, `missing COMM/SSND chunk`, `only mono/stereo supported`,
`unsupported AIFF-C compression`, `AIFF must be 16 or 24-bit`, `implausible sample rate`. `opd info`
reports them as `error: parse: <text>` (exit 4); the sampler reports `sampler: <text>` (render exit 5).
`opd info` prints `aiff: <path> rate=<n> ch=<n> frames=<n> duration=<x.xxx>s` for AIFF content.

## Synth voice v1 — additive (WP-0161)
Optional keys on a `"synth"` voice. A synth voice with **none** of `waveform`, `adsr`, `cutoff_hz` takes the
legacy path above **unchanged** (bit-identical to pre-WP-0161 renders; committed goldens prove it).
`resonance` alone also changes nothing (it only parameterises the filter). The three features are independent.

| key | type / range | default (= key absent) |
|---|---|---|
| `waveform` | string `"sine"`, `"saw"` or `"square"` | absent = the legacy timbre (fundamental + 0.35 octave partial); this is **not** the same as `"sine"` |
| `adsr` | object `{attack_s, decay_s, sustain, release_s}`; each inner key optional | absent = legacy envelope (`attack_ms` / `release_ms`, release inside the note, no tail). Present = ADSR below; inner defaults `attack_s` 0.005, `decay_s` 0.0, `sustain` 1.0, `release_s` 0.030 |
| `cutoff_hz` | number in `[20, 20000]` | absent = no filter |
| `resonance` | number (Q) in `[0.5, 20]` | 0.7071 |

`adsr` ranges: `attack_s`, `decay_s`, `release_s` in `[0, 10]` seconds; `sustain` in `[0, 1]`.
When `adsr` is present, `attack_ms`/`release_ms` are ignored. Using any of the four keys on a non-synth voice is an error.

**Numeric type.** Everything in the v1 path is computed in **IEEE double** (`double`), converted to `float` only
for the final per-sample value passed to the mixer (`add_stereo`). Constants: `PI = dsp::kPi_d()`. Only
`dsp::sind_`, `dsp::cosd_`, `dsp::powd_` are used for transcendental functions (`floor`/`ceil`/`min` are exact).
Exception: if `waveform` is absent but `adsr`/`cutoff_hz` is set, the oscillator is the legacy one, computed with
its exact legacy `float` expression (`dphi`, `sinf_`, `0.35f`, `kNorm`) and widened to double; likewise if `adsr`
is absent but `waveform`/`cutoff_hz` is set, the envelope is the legacy `float` envelope widened to double.

**Voice model.** Every note is an independent voice (polyphonic, no voice stealing, no choke): it starts at its
frame with oscillator phase 0 and all filter state 0 (this is the retrigger rule — a repeated or overlapping note
simply starts another voice, and all voices are summed by the mixer). `len = llround(note.dur × spb)` frames as
for every voice (`len <= 0` → nothing). Note-off is frame `len`. `n` below is the frame index relative to the
note start; `sr` is the project sample rate; `t = n / sr` as a double (`(double)n / (double)sr`).

**Pitch.** `f = 440.0 × powd_(2.0, (double)(midi_note − 69) / 12.0)` when the note has `midi_note`, else
`(double)voice.freq_hz`. `dt = f / sr`. When `waveform` is set and (`f <= 0` or `dt >= 0.5`) the note renders
nothing.

**Oscillator** (`waveform` set; amplitude 0.5 so a full-scale-safe peak of 0.5 before the filter/envelope).
`ph = f × (double)n / sr` (multiply first, then divide), `fr = ph − floor(ph)` ∈ [0,1).
- `sine`:   `w = sind_(2·PI·fr)`.
- `saw`:    `w = (2·fr − 1) − blep(fr)`.
- `square`: `t2 = fr + 0.5; if (t2 >= 1.0) t2 -= 1.0;  w = (fr < 0.5 ? 1.0 : −1.0) + blep(fr) − blep(t2)`.
- `blep(t)` (PolyBLEP, uses `dt` of the note): if `t < dt`: `x = t/dt; return x + x − x·x − 1`;
  else if `t > 1 − dt`: `x = (t − 1)/dt; return x·x + x + x + 1`; else `0`.
- oscillator output `x[n] = 0.5 × w`.

**Filter** (`cutoff_hz` set): a biquad low-pass (RBJ cookbook) in direct form I, in double. Per note:
`fc = min(cutoff_hz, 0.45 × sr)`, `w0 = 2·PI·fc / sr`, `cw = cosd_(w0)`, `sw = sind_(w0)`, `alpha = sw / (2·Q)`
(`Q` = `resonance`), `a0 = 1 + alpha`, then the normalised coefficients
`B0 = ((1 − cw)/2)/a0`, `B1 = (1 − cw)/a0`, `B2 = B0`, `A1 = (−2·cw)/a0`, `A2 = (1 − alpha)/a0`.
Difference equation: `y[n] = B0·x[n] + B1·x[n−1] + B2·x[n−2] − A1·y[n−1] − A2·y[n−2]` with
`x[−1] = x[−2] = y[−1] = y[−2] = 0`, applied to **every** frame the voice produces (including the release tail).
The filter sits before the envelope: `out[n] = y[n] × env[n]`. With no `cutoff_hz`, `y[n] = x[n]`.

**ADSR** (`adsr` present; `A, D, S, R` = `attack_s, decay_s, sustain, release_s`). Gate-held level at time `t`:
`g(t) = t / A` if `t < A`; else `1 − (1 − S)·(t − A)/D` if `t < A + D`; else `S`
(the divisions only occur when `A > 0` / `D > 0` respectively because of the comparisons).
- Frames `n < len`: `env[n] = g(n / sr)`.
- Release: `e_off = g(len / sr)` (the level at note-off, even if the attack/decay is unfinished there).
  The voice continues for `tail = ceil(R × sr)` further frames (`R = 0` → no tail, hard cut at `len`):
  for `n = len + k`, `0 <= k < tail`: `env[n] = e_off × (1 − (k / sr) / R)`. After that the voice is silent.
  The oscillator and filter keep running through the tail (the tail may extend past the project end — audio
  beyond the render window is clipped as for every voice).
With `adsr` absent the envelope is the legacy one and the voice ends at frame `len` (no tail).

**Output.** `s[n] = (float)(y[n] × env[n])`, written equally to L and R via the usual mixer path
(track gain, pan, velocity, clip gain, master gain unchanged).

**Canonical save.** `waveform` is written only when set, `adsr` only when present (then **all four** inner keys are
written, even at their defaults), `cutoff_hz` only when set, `resonance` only when it is not exactly `0.7071`.
Legacy synth voices write exactly the same bytes as before. A `null` value in a project file is a type error.

**Validation** (project parse; one code path for `load_project` and operations; checked in this order, first
failure wins, each only when its key is present). Messages are exact:
1. key present on a non-synth voice → `voice.<key> is only valid for synth voices` (`<key>` = the first present
   of `waveform`, `adsr`, `cutoff_hz`, `resonance`, in that order);
2. `waveform` not one of the three strings (including any non-string) → `voice.waveform must be sine|saw|square`;
3. `adsr` not an object → `voice.adsr must be an object`; an inner key other than the four →
   `voice.adsr has unknown key '<key>'`; then inner keys in the order `attack_s`, `decay_s`, `sustain`,
   `release_s`, each a JSON number in range, else `voice.adsr.attack_s must be a number in [0, 10]`,
   `voice.adsr.decay_s must be a number in [0, 10]`, `voice.adsr.sustain must be a number in [0, 1]`,
   `voice.adsr.release_s must be a number in [0, 10]`;
4. `cutoff_hz` not a number in `[20, 20000]` → `voice.cutoff_hz must be a number in [20, 20000]`;
5. `resonance` not a number in `[0.5, 20]` → `voice.resonance must be a number in [0.5, 20]`.
`opd` reports these like every other parse error (`error: parse: <message>`, exit 4).
`opd diff` is unchanged: a synth-params difference is `unsupported difference: tracks[i].voice`.

## Explicit pattern length — additive (WP-0162)
A pattern accepts an optional `"length_beats": <number>` (finite, `> 0`; any JSON number, integer or
float, is read as a double). It is the pattern's **extent** (loop period, in beats) and replaces the
derived `max(note.t + note.dur)` of §Arrangement. Rationale: a 16-step drum pattern whose last steps are
empty would otherwise loop early and drift out of the bar.

- **Validation** (model parse, so every entry point shares it): present but not a JSON number, zero,
  negative or non-finite → `pattern.length_beats must be a finite number > 0` (a literal such as `1e999` never
  gets that far: the JSON lexer itself rejects the overflow, exit 4 with the lexer's own message). There is
  no upper bound and no relation to the notes is checked.
- **Extent rule (normative):** `extent(pattern) = length_beats` if present, else the derived value
  (unchanged). Engine helper `double op::pattern_extent(const Pattern&)` in `engine/model.hpp`; the renderer
  and `opd opj2mid` both use it (MIDI export placement therefore matches the renderer).
- **Repeats:** with `length_beats` present `extent > 0` always, so `repeats` is always honored (even for an
  empty pattern, which then simply plays nothing `repeats` times). Absent: the existing rule
  (`extent <= 0` forces `repeats` to 1) is unchanged.
- **Notes are never truncated, wrapped or dropped by the length.** Every note of every repetition is
  placed at `base + r * extent + note.t` exactly as before, including notes whose `t + dur` runs past
  `length_beats` (their tail overlaps the next repetition) and notes with `t >= length_beats` (they play
  late, inside or after the next repetition). `length_beats` only changes the spacing between repetitions.
- **No arrangement:** legacy semantics apply (each pattern plays once from beat 0), so `length_beats` has no
  effect on the render.
- **Canonical save** emits `"length_beats"` (as a double, e.g. `4.0`) only when present; object keys are
  dumped alphabetically, so it lands before `"notes"` in the pattern object. Projects without the field save
  byte-identically to before (goldens prove it).
- `opd steps` default width and `opd diff` / operations: see `docs/spec/operations.md` §Pattern length operations.

## New project — additive (WP-0165)
`opd new <out.opj> [--bpm X] [--bars N] [--beats-per-bar N] [--sample-rate SR] [--track NAME:KIND[:PATH]]... [--force]`
creates a fresh project file from the engine function `op::new_project(const NewProjectSpec&)`
(`engine/model.hpp`; pure, Document-free, no I/O). The file is written with `op::save_project`, so its bytes are
exactly `op::project_bytes` of the result.

**Defaults:** `--bpm 120`, `--bars 4`, `--beats-per-bar 4`, `--sample-rate 48000`; no `--track` = the single track
`Synth` (kind `synth`). Options may appear in any order; `--track` repeats (declaration order = track order).

**Content (normative, fixes the bytes):** `format_version` 0, `seed` 42, `length_bars` = bars (as a double), no master
gain, tempo map, loop or attachments. For track i: `Track` (gain_db 0, pan 0, not muted/soloed, default voice
parameters of its kind — sine `freq_hz` 440, synth `attack_ms` 5 / `release_ms` 30, metronome `accent_every` 4,
sampler `path` = PATH verbatim, no trim); `Pattern` i = `{track: i, notes: []}`; arrangement clip i =
`{pattern: i, start_bar: 0, repeats: 1}`. Hence an empty-but-arranged project that renders silence of exactly
`total_samples_for` frames, loads, passes `opd info`, and accepts further ops (`add_note`, `add_clip`, ...).
Sampler PATH is stored as given (relative to the project file directory) and is NOT checked for existence.

**Option rules** (any violation: exit 2, `error: usage: <msg>`, nothing written; checked left to right before the
output path is examined):
- `--bpm`: whole argument parses as a finite number in (0, 1000] — else `bad --bpm value '<v>' (expected a number in (0, 1000])`.
- `--bars`: decimal digits only, 1..4096 — else `bad --bars value '<v>' (expected an integer 1..4096)`.
- `--beats-per-bar`: digits only, 1..32 — else `bad --beats-per-bar value '<v>' (expected an integer 1..32)`.
- `--sample-rate`: digits only, 8000..192000 — else `bad --sample-rate value '<v>' (expected an integer 8000..192000)`.
- `--track NAME:KIND[:PATH]`: split at the first `:` (NAME, non-empty) and the second `:` (KIND; PATH is everything
  after it and may itself contain `:`). Errors, each prefixed `bad --track '<v>': `: `expected NAME:KIND[:PATH]`
  (no `:` or empty NAME); `unknown kind '<k>' (expected sine, synth, sampler or metronome)`; `sampler needs a PATH`
  (missing or empty PATH); `PATH is only valid for sampler`. More than 64 tracks: `too many tracks (max 64)`.
- An option that needs a value as the last argument: `missing value for <option>`. Any other `--x`, or a second
  positional: `unknown arg '<arg>'`. No output path: `new needs <out.opj>`.

**Overwrite:** if `<out.opj>` exists (any file type) and `--force` is absent: exit 5,
`error: write: <out.opj> already exists (use --force)`, the existing file untouched. With `--force` it is replaced.
An unwritable path (e.g. missing parent directory) is exit 5, `error: write: cannot write '<out.opj>'`.
On success exit 0 and stdout `new: <out.opj> tracks=<n> bars=<n> bpm=<%g of bpm>`.

## Pattern swing — additive (WP-0166)
A pattern may carry an optional groove: `{"track": 0, "swing": 0.5, "notes": [...]}`. `swing` is a JSON number,
a **fraction** (not a percent), default `0`, valid range `[0, 0.75]`. It is a **render-time** effect only: notes
keep their stored `t`, `dur` and `vel`; nothing else in the project (extent, `step_grid`, MIDI export,
`opd analyze`) sees swing.

**Which notes are swung (normative).** A note is swung iff its own pattern-local `t` is *exactly* an odd 16th step:
`q = t * 4.0` (exact, power-of-two scale) satisfies `std::floor(q) == q` and `std::fmod(q, 2.0) == 1.0`
(so `t` = 0.25, 0.75, 1.25, ... qualify; 0.0, 0.5, 1.0, 0.3, 0.2500001 do not). The test uses the note's `t` inside its
pattern, never the clip-absolute position, so a clip placed at any `start_bar`/repeat offset swings the same notes.
All other notes are untouched (bit-identical to the unswung render).

**Shift.** `delay_beats = swing * 0.125` (swing times half of a 16th step of 0.25 beat; one double multiply,
exact scaling by a power of two). `swing = 0.0` is "off". A swung note's absolute beat position is the existing
placement expression plus the delay, one more double addition done last:
`t_abs = (offset_beats == 0.0 ? note.t : offset_beats + note.t)` and, if swung, `t_abs = t_abs + delay_beats`.

**Onset rounding (cites the existing rule).** The renderer's single onset rule is `TempoMap::at(beats)` in
`src/engine/render.cpp`: with no tempo map `llround(beats * spb)`, `spb = sample_rate * 60.0 / bpm`; with a tempo
map `segment.start_sample + llround((beats - segment.start_beats) * segment.spb)` where the segment is chosen by
the *position being converted*. The swung onset is exactly `TempoMap::at(t_abs)` for the shifted `t_abs` above
(so the tempo segment is chosen by the shifted position), and the local tempo for the note's length
(`TempoMap::spb_at`) is likewise taken at the shifted `t_abs`. `llround` is round-half-away-from-zero. The note
length in beats (`dur`) is not changed by swing. A swung note whose shifted onset is at/after the project window
end is dropped by the existing rule.

**Validation** (project parse, the same code path for `load_project` and operations; checked right after
`pattern.track`, before the notes): `swing` present but not a JSON number, or outside `[0, 0.75]` (including
negative values and anything above 0.75) → `pattern.swing must be a number in [0, 0.75]`. `null`, strings and
booleans give the same message. `-0.0` is accepted and treated as `0`.

**Canonical save** omits `"swing"` when it is exactly `0` (so every existing file and golden round-trips
byte-identically) and writes the number otherwise. Operation: `set_pattern_swing` (see `operations.md`).

## Sampler repitch — additive (WP-0167)
A sampler voice may declare the MIDI pitch its asset sounds at: `{"kind": "sampler", "path": "...", "root_note": 60}`.
`root_note` is optional, an integer `0..127`, valid **only on sampler voices**. A note with an explicit pitch `P`
(`note.note`, `0..127`) then plays the sample resampled by the ratio below. Everything else about playback
(velocity, pan, gain, clips, tempo map, mute/solo) is unchanged.

**Defaults / bit-exactness.** `root_note` absent → the sampler render path is the pre-WP-0167 path, byte for
byte (committed goldens prove it). With `root_note` present, a note whose pitch is absent (`note.note` omitted)
**uses the root: shift 0**, and a note with `P == root_note` is shift 0 too; **shift 0 takes the existing path**
(output identical to the same render with `root_note` absent). Only notes with `P != root_note` use the formula.

**Order of stages (normative).** (1) the asset is resampled to the project rate (WP-0138, unchanged), (2) the
trim slice `[i0, i1)` is cut from that buffer (WP-0156, unchanged, including its `sampler: trim slice is empty`
error), (3) the slice is repitched. Let `x[j] = buffer[i0 + j]` for `j = 0 .. L-1` with `L = i1 - i0`
(per channel for stereo assets; a mono asset feeds both outputs).

**Ratio (exact).** `shift = P - root_note` (integer, `-127..127`). Let `q = floor(shift / 12)` (floor division,
so `-1` for `shift = -1`) and `r = shift - 12*q` (`0..11`). `ratio = T[r] * 2^q` in `double`, where `2^q` is
applied by exact multiplication/division by `2.0` (no libm) and `T[r]` are these `double` constants
(`2^(r/12)` correctly rounded):
`T = {1.0, 1.0594630943592953, 1.122462048309373, 1.189207115002721, 1.2599210498948732, 1.3348398541700344,
1.4142135623730951, 1.4983070768766815, 1.5874010519681996, 1.681792830507429, 1.7817974362806785,
1.887748625363387}`. So `shift = +12` gives `ratio = 2.0` and `shift = -12` gives `ratio = 0.5` exactly.

**Read position and interpolation (exact).** For the note's output frame `n = 0, 1, 2, ...`
(relative to the note start): `pos = double(n) * ratio` (one multiplication per frame — NOT a running sum, so no
drift), `k = floor(pos)` (as a 64-bit integer), `frac = pos - double(k)` (in `[0, 1)`). The note is silent from
the first `n` for which `k + 1 >= L` (the interpolation needs `x[k+1]`; the last source frame is therefore
never the left neighbour). For every earlier `n` the output sample, per channel, is **linear interpolation in
double**: `y = double(x[k]) + frac * (double(x[k+1]) - double(x[k]))`, stored as `float(y)` and then mixed with
the same gain/pan/velocity path as today. The note sounds for `min(M, llround(note.dur * spb))` frames, where
`M` is the number of frames before the source runs out per the rule above — a note shorter than that gates the
sample (cut, no fade), a longer one is one-shot (no loop). A slice of `L = 1` yields silence (no error).

**Validation** (project parse, same path as `load_project` and operations; checked after the WP-0156 trim keys,
first failure wins, only when the key is present):
- `root_note` on a non-sampler voice → `voice.root_note is only valid for sampler voices`;
- `root_note` not a JSON integer in `0..127` (a string, a float such as `60.0` or `60.5`, `null`, `-1`, `128`)
  → `voice.root_note must be an integer 0..127`.
Canonical save emits `root_note` only when present (after `end_s`); legacy files round-trip byte-identically.
`opd diff` is unchanged: a root difference is reported as any voice change (`unsupported difference: tracks[i].voice`).

## Pattern ids — additive (WP-0179)

A pattern may carry an optional **name**: `{"track": 0, "id": "Kick A", "notes": [ ... ]}`. The id is a label
for humans, the GUI and the copilot. It does **not** replace index addressing: every operation argument, every
`clip.pattern` reference and `opd steps` still uses the 0-based position in `patterns`.

```jsonc
"patterns": [ { "track": 0, "id": "Kick A", "notes": [ ... ] } ]   // "id" optional, unique when set
```

- **Model:** `std::string id;` on `op::Pattern` (`engine/model.hpp`). `""` = unnamed (the default; the key is
  absent in JSON).
- **Value rules:** a JSON string, **non-empty**, at most **64 bytes** (UTF-8 counted as bytes by
  `std::string::size()`), and **unique across the patterns of one project** that declare an id. Comparison is
  exact byte equality, case-sensitive; no character restrictions (quotes and non-ASCII are allowed and are
  JSON-escaped on save).
- **Validation** at parse (`load_project`, the same code path operations use), checked right after
  `pattern.track` and before `swing`; first failure wins; each line below is the whole error string:
  1. `id` present but not a JSON string (number, `null`, boolean, object, array) → `pattern.id must be a string`
  2. empty string → `pattern.id must not be empty`
  3. longer than 64 bytes → `pattern.id must be at most 64 bytes`
  4. equal to the id of an **earlier** pattern of the same project → `pattern.id '<id>' is already in use`
     (the id verbatim inside single quotes; patterns are scanned in array order, so the later one is rejected)
- **Canonical save** writes `"id"` only when it is non-empty. Object keys dump alphabetically, so inside a
  pattern object `id` lands first: `id`, `length_beats`, `notes`, `swing`, `track`. Projects without ids save
  byte-for-byte as before (all goldens unchanged).
- **Semantics:** identity only. The renderer, MIDI import/export, `opd lint`, `opd steps` and the mixer never
  read it: a project renders bit-identically with or without pattern ids, and duplicating a pattern changes no
  sample of an arrangement that does not reference the copy.
- **Operations:** `duplicate_pattern {src, new_id}` (see `operations.md` §Pattern duplicate operation) and
  `add_pattern` (a pattern JSON may carry `id`; a duplicate one is refused by rule 4 above). No rename
  operation exists yet, so `opd diff` reports an id difference on a shared pattern as
  `unsupported difference: patterns[i].id`.
