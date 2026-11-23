---
id: WP-0197
title: Phase 1 exit demo — "Exit Beat", a 3-minute beat built only with `opd new` + an op journal, plus bit-exact golden, harness script and ear-check pack
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: ear-check + demo
touches: [spec:render-harness, new:harness/make_phase1_song.sh, harness/make_earcheck.sh, new:tests/fixtures/songs/phase1_beat.opjl, new:tests/fixtures/songs/phase1_beat.opj, new:tests/fixtures/songs/phase1_beat.wav.sha256, new:tests/fixtures/songs/kits/kick.wav]
---

## Context - why this exists

Phase 1's exit criterion is *"you can arrange a 3-min beat with samples + one bundled synth"*. This package
proves it as an artifact instead of a claim: one op journal that turns the canonical `opd new` project into a
finished 3-minute arrangement — sampler drums, one `synth_v1` line, clip fades and a trimmed clip, a muted
alternate layer, solo verified on the same song, and the master limiter on the bounce — plus a bit-exact golden
and an ear-check pack (mix + stems) for the owner. **No project JSON is hand-written**: every edit is one
operation from the existing op set.

This package adds **no engine code, no new op, no CLI flag**. It reuses what exists (read it first, do not
reimplement or change it): `opd new` / `opd apply` / `opd render --seed --stems-dir --limit-dbfs` in
`src/cli/main.cpp`; the ops and their JSON shapes in `docs/spec/operations.md` (`set_step`, `add_note`,
`duplicate_pattern`, `add_pattern`, `set_pattern_length`, `set_track_gain/pan/mute/solo`, `set_master_gain`,
`set_sampler_trim`, `set_sampler_root`, `set_synth_params`, `add_clip`, `remove_clip`, `set_clip_fades`,
`set_clip_trim`); the kit hit `tests/fixtures/goldens/kits/kick.wav`; and the harness style of
`harness/test_cli_errors.sh`. **Every gap you find in existing ops goes into a `PROPOSE:` note in the handoff —
never into a fix.**

## Spec - normative (this task file is the contract)

### Files (exactly these; stage no others)
- `tests/fixtures/songs/phase1_beat.opjl` (new) — the op journal, **byte-for-byte the content below**.
- `tests/fixtures/songs/phase1_beat.opj` (new, generated) — the project that `opd new` + that journal produce.
- `tests/fixtures/songs/phase1_beat.wav.sha256` (new, generated) — SHA-256 of the golden mix render (the 34 MB pcm16 `.wav` itself is NOT committed).
- `tests/fixtures/songs/kits/kick.wav` (new, generated) — a copy of `tests/fixtures/goldens/kits/kick.wav`, so
  `tests/fixtures/songs/phase1_beat.opj` renders in place.
- `harness/make_phase1_song.sh` (new, mode `755`) — build / check / ear-check, contract below.
- `harness/make_earcheck.sh` — purely **additive**: the four lines given below, inserted directly after the
  line `"$ROOT/build/opd" render "$G/panorama.opj"  -o "$OUT/04_panorama_mix.wav"`. No existing line changes.
- `docs/spec/render-harness.md` — a section documenting the exit song is appended (the runner ships the full
  replacement file; do not restructure the existing text).

The two `.wav` fixtures come from `bash harness/make_phase1_song.sh --build` — nothing but the journal is
hand-written. **Owner decision (2026-11-23): do NOT commit `phase1_beat.wav`** (34 MB would stay in git history
forever). Ship text only: commit the journal, the `.opj`, the small `kits/kick.wav` copy, and a
`tests/fixtures/songs/phase1_beat.wav.sha256` file holding the SHA-256 of the `--build` render; `--check` must
compare a fresh render against that hash. `--build` renders the mix into a `mktemp -d` dir (never into
`tests/fixtures/`, or the uncommitted 34 MB file gets swept onto a salvage branch) and writes only the `.sha256`
into the repo. The hidden check verifies a fresh render against `phase1_beat.wav.sha256`. Every fixture regenerates from the journal by `--build`, and no gate
depends on the 34 MB binary being present.

Nothing else: no `src/`, no `CMakeLists.txt`, no `tests/test_*.cpp`, no other fixture, no `.github/`.

### Why these fixtures live in `tests/fixtures/songs/`, NOT in `tests/fixtures/goldens/`
`harness/run_goldens.sh` renders **every** `tests/fixtures/goldens/*.opj` twice per CI run and needs a
committed `.opj` + `.wav` pair. This mix is 34 MB, so it keeps its own determinism contract in
`harness/make_phase1_song.sh --check`. Do not move it into `goldens/`, and do not touch `run_goldens.sh`.

### The song (fixed; the journal below is the only definition you need)
"Exit Beat": 96 BPM, 4/4, **72 bars = exactly 180.000 s at 48 kHz**, seed 42 in the project. Five tracks:
`Kick`, `Snare`, `Hat`, `Perc` (all sampler, all `kits/kick.wav`) and `Bass` (the single `synth_v1` line).
Drums are that one kit hit re-voiced by `set_sampler_trim` + `set_sampler_root`; `Perc` is an alternate layer
kept **muted**; the limiter runs at render time (`--limit-dbfs -1.0`), never in the model.

The journal must be applied to a project created with exactly this command (the script does it):

```bash
opd new phase1_beat.opj --bpm 96 --bars 72 \
  --track Kick:sampler:kits/kick.wav --track Snare:sampler:kits/kick.wav \
  --track Hat:sampler:kits/kick.wav --track Bass:synth --track Perc:sampler:kits/kick.wav
opd apply phase1_beat.opj phase1_beat.opjl      # prints: apply: 114 op(s) -> phase1_beat.opj
```

Journal — copy every line exactly (blank lines and `#` comments are ignored by `opd apply`, keep them):

```jsonl
# WP-0197 - "Exit Beat": the Phase 1 exit-criterion song as an op journal (no hand-edited JSON).
# Build it with exactly:
#   opd new phase1_beat.opj --bpm 96 --bars 72 \
#     --track Kick:sampler:kits/kick.wav --track Snare:sampler:kits/kick.wav \
#     --track Hat:sampler:kits/kick.wav --track Bass:synth --track Perc:sampler:kits/kick.wav
#   opd apply phase1_beat.opj phase1_beat.opjl
# 96 BPM, 4/4, 72 bars = 180.000 s at 48 kHz. Sampler drums are the one committed kit hit
# (kits/kick.wav) re-voiced by trim and repitch; Bass is the single synth_v1 line; Perc is an
# alternate layer kept muted. Render with --limit-dbfs -1.0 for the master limiter.

# ---- mixer -------------------------------------------------------------------
{"op":"set_track_gain","index":0,"gain_db":-0.5}
{"op":"set_track_gain","index":1,"gain_db":-3.0}
{"op":"set_track_pan","index":1,"pan":-0.2}
{"op":"set_track_gain","index":2,"gain_db":-9.0}
{"op":"set_track_pan","index":2,"pan":0.25}
{"op":"set_track_gain","index":3,"gain_db":-4.5}
{"op":"set_track_gain","index":4,"gain_db":-6.0}
{"op":"set_track_pan","index":4,"pan":-0.35}
{"op":"set_track_mute","index":4,"mute":true}
{"op":"set_master_gain","gain_db":1.0}

# ---- voices ------------------------------------------------------------------
{"op":"set_sampler_trim","index":1,"start_s":0.0,"end_s":0.12}
{"op":"set_sampler_trim","index":2,"start_s":0.0,"end_s":0.03}
{"op":"set_sampler_root","index":2,"root_note":60}
{"op":"set_sampler_root","index":4,"root_note":60}
{"op":"set_synth_params","index":3,"waveform":"saw","adsr":{"attack_s":0.01,"decay_s":0.12,"sustain":0.6,"release_s":0.25},"cutoff_hz":900.0,"resonance":0.8}

# ---- patterns 0..4 (created empty by opd new, one per track) -------------------
# pattern 0 - Kick: four on the floor
{"op":"set_step","pattern":0,"step":0,"on":true}
{"op":"set_step","pattern":0,"step":4,"on":true}
{"op":"set_step","pattern":0,"step":8,"on":true}
{"op":"set_step","pattern":0,"step":12,"on":true}
# pattern 1 - Snare: backbeat
{"op":"set_step","pattern":1,"step":4,"on":true}
{"op":"set_step","pattern":1,"step":12,"on":true}
# pattern 2 - Hat: eighths, pitched up an octave (+12 from root_note 60)
{"op":"set_step","pattern":2,"step":0,"on":true,"pitch":72}
{"op":"set_step","pattern":2,"step":2,"on":true,"pitch":72}
{"op":"set_step","pattern":2,"step":4,"on":true,"pitch":72}
{"op":"set_step","pattern":2,"step":6,"on":true,"pitch":72}
{"op":"set_step","pattern":2,"step":8,"on":true,"pitch":72}
{"op":"set_step","pattern":2,"step":10,"on":true,"pitch":72}
{"op":"set_step","pattern":2,"step":12,"on":true,"pitch":72}
{"op":"set_step","pattern":2,"step":14,"on":true,"pitch":72}
# pattern 3 - Bass: two-bar riff (extent 8 beats), A minor
{"op":"add_note","pattern":3,"note":{"t":0.0,"dur":0.75,"note":45}}
{"op":"add_note","pattern":3,"note":{"t":1.0,"dur":0.5,"note":45,"vel":0.8}}
{"op":"add_note","pattern":3,"note":{"t":1.75,"dur":0.25,"note":41}}
{"op":"add_note","pattern":3,"note":{"t":2.25,"dur":0.75,"note":38}}
{"op":"add_note","pattern":3,"note":{"t":3.25,"dur":0.75,"note":38,"vel":0.8}}
{"op":"add_note","pattern":3,"note":{"t":4.0,"dur":0.75,"note":43}}
{"op":"add_note","pattern":3,"note":{"t":5.0,"dur":0.5,"note":43,"vel":0.8}}
{"op":"add_note","pattern":3,"note":{"t":5.75,"dur":0.25,"note":41}}
{"op":"add_note","pattern":3,"note":{"t":6.25,"dur":0.75,"note":40}}
{"op":"add_note","pattern":3,"note":{"t":7.25,"dur":0.5,"note":36}}
# pattern 4 - Perc: offbeat shaker part (muted track, kept for later)
{"op":"set_step","pattern":4,"step":3,"on":true,"pitch":67}
{"op":"set_step","pattern":4,"step":7,"on":true,"pitch":67}
{"op":"set_step","pattern":4,"step":11,"on":true,"pitch":67}
{"op":"set_step","pattern":4,"step":14,"on":true,"pitch":67}

# ---- patterns 5..6 --------------------------------------------------------------
# pattern 5 - Hat16: sixteenths, from the eighths copy
{"op":"duplicate_pattern","src":2,"new_id":"hat16"}
{"op":"set_step","pattern":5,"step":1,"on":true,"pitch":72}
{"op":"set_step","pattern":5,"step":3,"on":true,"pitch":72}
{"op":"set_step","pattern":5,"step":5,"on":true,"pitch":72}
{"op":"set_step","pattern":5,"step":7,"on":true,"pitch":72}
{"op":"set_step","pattern":5,"step":9,"on":true,"pitch":72}
{"op":"set_step","pattern":5,"step":11,"on":true,"pitch":72}
{"op":"set_step","pattern":5,"step":13,"on":true,"pitch":72}
{"op":"set_step","pattern":5,"step":15,"on":true,"pitch":72}
# pattern 6 - Roll: sixteenth crescendo into the finale
{"op":"add_pattern","pattern":{"track":1,"id":"roll","notes":[]}}
{"op":"set_step","pattern":6,"step":0,"on":true,"velocity":0.2}
{"op":"set_step","pattern":6,"step":1,"on":true,"velocity":0.25}
{"op":"set_step","pattern":6,"step":2,"on":true,"velocity":0.3}
{"op":"set_step","pattern":6,"step":3,"on":true,"velocity":0.35}
{"op":"set_step","pattern":6,"step":4,"on":true,"velocity":0.4}
{"op":"set_step","pattern":6,"step":5,"on":true,"velocity":0.45}
{"op":"set_step","pattern":6,"step":6,"on":true,"velocity":0.5}
{"op":"set_step","pattern":6,"step":7,"on":true,"velocity":0.55}
{"op":"set_step","pattern":6,"step":8,"on":true,"velocity":0.6}
{"op":"set_step","pattern":6,"step":9,"on":true,"velocity":0.65}
{"op":"set_step","pattern":6,"step":10,"on":true,"velocity":0.7}
{"op":"set_step","pattern":6,"step":11,"on":true,"velocity":0.75}
{"op":"set_step","pattern":6,"step":12,"on":true,"velocity":0.8}
{"op":"set_step","pattern":6,"step":13,"on":true,"velocity":0.85}
{"op":"set_step","pattern":6,"step":14,"on":true,"velocity":0.9}
{"op":"set_step","pattern":6,"step":15,"on":true,"velocity":0.95}
# ---- pattern lengths -----------------------------------------------------------
# Explicit loop periods: without them a pattern's extent is max(t + dur) of its notes (3.25 beats for
# the kick, 3.75 for the eighths hat), so every repeat would slip against the bar grid.
{"op":"set_pattern_length","pattern":0,"length_beats":4.0}
{"op":"set_pattern_length","pattern":1,"length_beats":4.0}
{"op":"set_pattern_length","pattern":2,"length_beats":4.0}
{"op":"set_pattern_length","pattern":3,"length_beats":8.0}
{"op":"set_pattern_length","pattern":4,"length_beats":4.0}
{"op":"set_pattern_length","pattern":5,"length_beats":4.0}
{"op":"set_pattern_length","pattern":6,"length_beats":4.0}
# ---- arrangement: drop the five one-bar clips opd new wrote, then the song ------
{"op":"remove_clip","index":4}
{"op":"remove_clip","index":3}
{"op":"remove_clip","index":2}
{"op":"remove_clip","index":1}
{"op":"remove_clip","index":0}
# intro (bars 0-8): hats swell in
{"op":"add_clip","clip":{"pattern":2,"start_bar":0.0,"repeats":8}}
# beat A (bars 8-24)
{"op":"add_clip","clip":{"pattern":0,"start_bar":8.0,"repeats":16}}
{"op":"add_clip","clip":{"pattern":1,"start_bar":8.0,"repeats":16}}
{"op":"add_clip","clip":{"pattern":2,"start_bar":8.0,"repeats":16}}
{"op":"add_clip","clip":{"pattern":3,"start_bar":8.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":4,"start_bar":8.0,"repeats":16}}
# break (bars 24-32): hats and the first half of the riff
{"op":"add_clip","clip":{"pattern":2,"start_bar":24.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":3,"start_bar":24.0,"repeats":2}}
# drop (bars 32-48): sixteenth hats
{"op":"add_clip","clip":{"pattern":0,"start_bar":32.0,"repeats":16}}
{"op":"add_clip","clip":{"pattern":1,"start_bar":32.0,"repeats":16}}
{"op":"add_clip","clip":{"pattern":5,"start_bar":32.0,"repeats":16}}
{"op":"add_clip","clip":{"pattern":3,"start_bar":32.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":4,"start_bar":32.0,"repeats":16}}
# half-time (bars 48-56) with the roll building from bar 52
{"op":"add_clip","clip":{"pattern":1,"start_bar":48.0,"repeats":4}}
{"op":"add_clip","clip":{"pattern":2,"start_bar":48.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":3,"start_bar":48.0,"repeats":4}}
{"op":"add_clip","clip":{"pattern":6,"start_bar":52.0,"repeats":2}}
# finale (bars 56-64)
{"op":"add_clip","clip":{"pattern":0,"start_bar":56.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":1,"start_bar":56.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":5,"start_bar":56.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":3,"start_bar":56.0,"repeats":4}}
{"op":"add_clip","clip":{"pattern":4,"start_bar":56.0,"repeats":8}}
# outro (bars 64-72): hats trimmed to end early and faded out under the last riff bar
{"op":"add_clip","clip":{"pattern":2,"start_bar":64.0,"repeats":8}}
{"op":"add_clip","clip":{"pattern":3,"start_bar":64.0,"repeats":2}}
# fades: clip indices are the append order above (the five opd new clips were removed first)
{"op":"set_clip_fades","index":0,"fade_in_beats":4.0,"fade_out_beats":0.0}
{"op":"set_clip_fades","index":1,"fade_in_beats":0.0,"fade_out_beats":2.0}
{"op":"set_clip_fades","index":2,"fade_in_beats":0.0,"fade_out_beats":2.0}
{"op":"set_clip_fades","index":4,"fade_in_beats":2.0,"fade_out_beats":0.0}
{"op":"set_clip_fades","index":6,"fade_in_beats":0.0,"fade_out_beats":2.0}
{"op":"set_clip_fades","index":13,"fade_in_beats":2.0,"fade_out_beats":0.0}
{"op":"set_clip_fades","index":15,"fade_in_beats":4.0,"fade_out_beats":0.0}
{"op":"set_clip_fades","index":22,"fade_in_beats":0.0,"fade_out_beats":8.0}
# clip trim: the outro hat clip plays source beats 2..28 of its 32-beat unrolled pattern
{"op":"set_clip_trim","index":22,"offset_beats":2.0,"length_beats":26.0}

```

Resulting structure (what the hidden check reads back from `tests/fixtures/songs/phase1_beat.opj`):
`bpm 96.0`, `length_bars 72.0`, `beats_per_bar 4`, `sample_rate 48000`, `seed 42`, `master_gain_db 1.0`;
5 tracks named `Kick, Snare, Hat, Bass, Perc` in that order with gains `-0.5, -3.0, -9.0, -4.5, -6.0`, pans
`0.0, -0.2, 0.25, 0.0, -0.35`, `mute: true` on `Perc` only and no track soloed; sampler voices trimmed to
`end_s 0.12` (Snare) and `end_s 0.03` (Hat), `root_note 60` on Hat and Perc; the Bass voice is
`waveform "saw"` with `cutoff_hz 900.0` and an `adsr` of `attack_s 0.01, decay_s 0.12, sustain 0.6, release_s 0.25`.
7 patterns (`hat16` on the Hat track, `roll` on the Snare track, the rest unnamed), every one with an explicit
`length_beats` (4.0; the Bass riff is 8.0 — without them a pattern's extent is `max(t + dur)` and every repeat
slips against the bar grid). 24 clips: fades on clip indices `0 (+4.0), 1 (-2.0), 2 (-2.0), 4 (+2.0), 6 (-2.0),
13 (+2.0), 15 (+4.0), 22 (-8.0)` (values in beats, absent side omitted on save) and clip **22** trimmed with
`offset_beats 2.0, length_beats 26.0`.

### Golden render flags (fixed)
```bash
opd render phase1_beat.opj -o phase1_beat.wav --seed 1337 --limit-dbfs -1.0
```
Run from `tests/fixtures/songs/` (the sample path is project-relative). Measured on the reference rig:
`8640000` frames, sample peak `-0.9999 dBFS`, integrated `-15.6 LUFS`; the same render **without**
`--limit-dbfs` peaks at `0.0 dBFS` (it clips — that is why the bounce is limited).

### `harness/make_phase1_song.sh` — exact contract
```
usage: harness/make_phase1_song.sh [--build | --check | --earcheck] [path/to/opd]
```
Default mode `--build`; default opd `$ROOT/build/opd`; unknown flag → print the usage and exit 2. Bash, no
numpy, `mktemp -d` scratch with a cleanup trap (never a fixed `/tmp/op_*` path), no wall-clock anywhere.

- `--build` — refresh `tests/fixtures/songs/kits/kick.wav` from `tests/fixtures/goldens/kits/kick.wav`, rebuild
  `tests/fixtures/songs/phase1_beat.opj` from the exact `opd new` command + the journal, then render the golden
  `.wav` with the flags above (so a checkout that carries only text can rebuild every fixture). Prints what it
  built; exit 0 on success, 1 on failure.
- `--check` — print one `PASS <name>` / `FAIL <name>` line per assertion and a final line
  `make_phase1_song: N failure(s)`; exit code = N. It must verify, at least: the journal applies cleanly to
  the exact `opd new` project and reproduces the committed `.opj` **byte for byte**; two renders of it with the
  golden flags are bit-identical; the committed `.wav` equals a fresh render — and if the `.wav` is missing
  (a text-only patch series cannot carry 34 MB) regenerate it from the journal instead of failing; exactly
  five stems named `0_kick.wav 1_snare.wav 2_hat.wav 3_bass.wav 4_perc.wav`; the muted `Perc` stem all-zero and
  every other stem non-silent; the limited mix's `opd analyze` `peak_dbfs` sits in `[-3.0, -0.9]` while the
  un-limited render's is at or above `-0.3`, and the two renders differ byte for byte; soloing track 3
  (`{"op":"set_track_solo","index":3,"solo":true}` applied to a copy) leaves only the Bass stem non-silent; the
  intro fade-in and the trimmed outro clip are audible (a twin journal that zeroes every `set_clip_fades` and
  clears the trim with `null`s must change the render, make the first 100 ms of the mix at least 8× quieter than
  the same window of the twin, and leave the last second of the mix near-silent — peak below `20` of 32767 —
  while the un-trimmed twin peaks above `2000` there).
- `--earcheck` — write the owner's pack: `demos/earcheck/07_exit_beat_mix.wav` (byte-identical to the golden),
  `demos/earcheck/phase1_exit_stems/` (the five stems) and `demos/earcheck/08_exit_beat_bass_solo.wav`
  (the solo variant, same flags). Print where it wrote them; exit 0 on success.

### `harness/make_earcheck.sh` — the four additive lines
```bash
# WP-0197: the Phase 1 exit-criterion song "Exit Beat" (3-minute beat, built from `opd new` + an op
# journal in tests/fixtures/songs/). Writes 07_exit_beat_mix.wav (+ phase1_exit_stems/ and
# 08_exit_beat_bass_solo.wav) into $OUT; the mix is byte-identical to tests/fixtures/songs/phase1_beat.wav.
bash "$ROOT/harness/make_phase1_song.sh" --earcheck "$ROOT/build/opd" || echo "WARN: exit-song pack failed"
```

### Gaps found while drafting — `PROPOSE:` only, do NOT fix them in this package
- **One drum hit.** The repo kit has exactly one sample (`tests/fixtures/goldens/kits/kick.wav`), so Snare, Hat
  and Perc here are that kick re-voiced with `set_sampler_trim` + `set_sampler_root`. It works and it is honest
  about the engine's limits, but a real Phase 1 demo wants four hits. PROPOSE a follow-up package adding a small
  CC0 kit (kick / snare / closed hat / clap) under `tests/fixtures/goldens/kits/` with licences recorded.
- **CI wiring.** `harness/run_goldens.sh` runs in CI; this song's determinism contract lives in
  `make_phase1_song.sh --check` and nothing runs it automatically. PROPOSE adding one line next to the goldens
  step in `.github/workflows/ci.yml` (this package leaves `.github/` untouched on purpose).
- **No arrangement-level mute/solo.** Mute and solo are per track, so "drop the Perc layer for the break" needs
  a clip-level or section-level mute. This song keeps `Perc` muted for the whole take instead. PROPOSE discussing
  a clip-scoped mute op before anyone builds it.

## Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness anywhere in the journal or scripts; the render is fixed by `--seed 1337`.
Same input → byte-identical WAVs, run to run and across x86_64/aarch64. No engine code changes, so `-Werror`
and `-DDZ_SANITIZE=ON` must stay untouched and green.

## Acceptance criteria
- [x] Every file listed above exists; `tests/fixtures/songs/phase1_beat.opjl` is exactly the journal in this task
      file, and `opd new` (exact command) + `opd apply` reproduces `tests/fixtures/songs/phase1_beat.opj`
      **byte for byte** (`apply: 114 op(s) -> phase1_beat.opj`).
- [x] The committed `.opj` has the structure listed above (5 tracks / 7 patterns / 24 clips, mute on `Perc`,
      explicit `length_beats` everywhere, fades and the clip-22 trim).
- [x] `opd render phase1_beat.opj -o x.wav --seed 1337 --limit-dbfs -1.0` writes exactly **8640000 frames**
      (180.000 s), renders bit-identically twice, has an `opd analyze` `peak_dbfs` in `[-3.0, -0.9]`, while the
      same render without `--limit-dbfs` peaks at or above `-0.3 dBFS` and is byte-different.
- [x] Stems: exactly `0_kick.wav 1_snare.wav 2_hat.wav 3_bass.wav 4_perc.wav`; `4_perc.wav` all-zero; the
      other four non-silent (peak > 2000 of 32767). With track 3 soloed, only `3_bass.wav` is non-silent.
- [x] `bash harness/make_phase1_song.sh --check` exits **0** and prints its PASS lines; `--earcheck` writes the
      three pack paths under `demos/earcheck/`; `harness/make_earcheck.sh` still runs end to end.
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `harness/run_goldens.sh` (all 16
      existing fixtures unchanged — no golden was added to `goldens/`), `harness/test_cli_errors.sh`,
      `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests`.
- [x] Handoff report at the bottom includes the commands you ran with their output, and a `PROPOSE:` note for
      every gap you found in existing ops (e.g. one drum hit in the repo kit; no per-section mute/solo).

## Out of scope - do NOT touch
- `ROADMAP.md`, `src/` (engine, CLI, app), `CMakeLists.txt`, `tests/test_*.cpp`, `ai-sidecar/`, `.github/`.
- Every existing file under `tests/fixtures/` except the new `tests/fixtures/songs/` tree — in particular
  `tests/fixtures/goldens/**` is read-only here (copy from it, never edit or add to it).
- `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh` and every other
  harness script except the four additive lines in `harness/make_earcheck.sh`.
- No new operation, no new CLI flag, no model field, no engine change — a gap is a `PROPOSE:` note.
- Do not commit anything under `demos/` (gitignored) and do not add a second 34 MB file anywhere.

## Git
- Branch `wp-0197-phase-1-exit-demo` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0197: Phase 1 exit demo — Exit Beat as opd new + op journal, golden + ear-check pack`.
  Stage explicit paths only (never `git add -A`). Include `TASKS/WP-0197-phase-1-exit-demo.md` with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the files this package allows)
- `tests/fixtures/songs/phase1_beat.opjl` (new) — the journal, byte-for-byte the fenced content of this task
  file (152 lines / 9330 bytes, fence included; `grep -c '^{'` = 114 op lines).
- `tests/fixtures/songs/phase1_beat.opj` (new, generated by `--build`) — 9210 bytes; structure below.
- `tests/fixtures/songs/phase1_beat.wav.sha256` (new, generated) — `ea32b748…fdca` (bare hex digest, one line).
- `tests/fixtures/songs/kits/kick.wav` (new) — byte copy of `tests/fixtures/goldens/kits/kick.wav` (57644 B).
- `harness/make_phase1_song.sh` (new, mode 755) — `--build` / `--check` / `--earcheck` per contract; bash +
  opd + coreutils only (peaks measured with `od -An -td2` over the 44-byte canonical pcm16 header),
  `mktemp -d` scratch + cleanup trap, no wall clock, golden mix rendered into scratch — never into
  `tests/fixtures/` (only the `.sha256` lands in the repo).
- `harness/make_earcheck.sh` — purely additive: the four spec'd lines inserted directly after the
  `04_panorama_mix.wav` line; `git diff` shows 4 insertions, 0 deletions.
- `docs/spec/render-harness.md` — the runner-installed replacement (shipped modified in the working tree);
  committed unchanged, no rewording.
- No `src/`, no `CMakeLists.txt`, no `tests/test_*.cpp`, no `.github/`, nothing under `tests/fixtures/goldens/`.
  `phase1_beat.wav` (34 MB) is NOT committed (owner decision); `demos/` stays gitignored.

Branch note: `wp-0197-phase-1-exit-demo` already existed pointing at a discarded earlier attempt
(`e6d2bb3`); recreated at the integration base `d7e3fa3` as this package's contract requires
(`git checkout -B`). The old commit stays only in the reflog — nothing was pushed or merged.
Curiosity check: the prior attempt's `.opj` was byte-identical to the one this journal regenerates.
`wpdraft6/` was already untracked in the working tree when the task started (runner/drafting artifact,
outside this package's scope) — left untouched and unstaged.

### Acceptance criteria — commands and real output (aarch64 DGX Spark, gcc 13)

Build (dz-build kit: unique dir, -j4, shared lock, ccache; log has 0 `warning`/`error` lines):
```
$ bash agent-build-kit/loop/tools/dz_build.sh openpattern --target opd --target op_tests --out .builds/wp0197
/home/shubhankar_dgx/qwen-scratch/.builds/wp0197          # exit 0; [100%] Built target op_tests
$ grep -ci "warning\|error" .builds/wp0197/dz_build.log
0
```

`--build` (kit copy, journal → .opj, golden render in scratch, digest only into the repo):
```
$ bash harness/make_phase1_song.sh --build
built kit copy: tests/fixtures/songs/kits/kick.wav (from tests/fixtures/goldens/kits/kick.wav)
apply: 114 op(s) -> /tmp/tmp.mFWAYTIScn/phase1_beat.opj
built project: tests/fixtures/songs/phase1_beat.opj
rendered golden mix in scratch: 8640000 frames, peak -0.9999 dBFS, -15.6326 LUFS (not committed by design)
wrote digest: tests/fixtures/songs/phase1_beat.wav.sha256 = ea32b748c68ff65aa501431fe408ede892387c4a1030852082592b31df85fdca
exit=0
```
8640000 frames / −0.9999 dBFS / −15.63 LUFS match this package's reference-rig numbers.

`--check` (full determinism + structure + audibility contract):
```
$ bash harness/make_phase1_song.sh --check
PASS kit_copy_matches_goldens
PASS opd_new_canonical_project
PASS journal_applies_cleanly_114_ops
PASS opj_matches_journal_byte_for_byte
PASS golden_render_bit_exact_twice
PASS mix_frames_8640000
PASS golden_render_matches_committed_sha256
PASS wav_absent_regenerated_from_journal
PASS stems_exactly_five_expected_names
PASS stem_perc_silent_track_muted
PASS stem_0_kick_audible (peak 19175 > 2000)
PASS stem_1_snare_audible (peak 16451 > 2000)
PASS stem_2_hat_audible (peak 8474 > 2000)
PASS stem_3_bass_audible (peak 10415 > 2000)
PASS limited_mix_peak_in_range (peak_dbfs -0.9999)
PASS unlimited_render_peaks_at_or_above_-0.3 (peak_dbfs 0.0000)
PASS limiter_changes_render_bytes
PASS solo_track3_leaves_only_bass_audible
PASS twin_journal_zeroes_fades_and_trim
PASS twin_render_differs
PASS intro_fade_in_audible (mix head 36 vs twin 8474, ratio >= 8x)
PASS outro_last_second_near_silent_in_mix (peak 0 < 20)
PASS outro_last_second_audible_in_untrimmed_twin (peak 8474 > 2000)
make_phase1_song: 0 failure(s)
exit=0
```
Usage contract: `--frobnicate` and a second positional arg both print the usage line and exit 2.

`--earcheck` + pack (and the ear-check mix is digest-identical to the committed sha256):
```
$ bash harness/make_phase1_song.sh --earcheck
exit-song ear-check pack written:
  demos/earcheck/07_exit_beat_mix.wav          (byte-identical to the golden render)
  demos/earcheck/phase1_exit_stems/            (0_kick.wav 1_snare.wav 2_hat.wav 3_bass.wav 4_perc.wav)
  demos/earcheck/08_exit_beat_bass_solo.wav    (track 3 soloed, same flags)
exit=0
$ sha256sum demos/earcheck/07_exit_beat_mix.wav
ea32b748c68ff65aa501431fe408ede892387c4a1030852082592b31df85fdca  demos/earcheck/07_exit_beat_mix.wav
$ bash harness/make_earcheck.sh          # end to end, four new lines included
… 04_panorama_mix.wav rendered …
exit-song ear-check pack written: (the three paths above)
ear-check pack ready in demos/earcheck/
make_earcheck exit=0
```

`.opj` structure (python3 stdlib json dump of the committed file):
```
bpm 96.0 | length_bars 72.0 | beats_per_bar 4 | sample_rate 48000 | seed 42 | master_gain_db 1.0
Kick gain -0.5 pan 0.0  mute -    | sampler kits/kick.wav
Snare gain -3.0 pan -0.2 mute -    | sampler end_s 0.12
Hat   gain -9.0 pan 0.25 mute -    | sampler end_s 0.03 root 60
Bass  gain -4.5 pan 0.0  mute -    | synth saw cutoff 900.0 adsr {attack_s 0.01, decay_s 0.12, sustain 0.6, release_s 0.25}
Perc  gain -6.0 pan -0.35 mute True | sampler root 60
patterns: 7  [('', 0, 4.0), ('', 1, 4.0), ('', 2, 4.0), ('', 3, 8.0), ('', 4, 4.0), ('hat16', 2, 4.0), ('roll', 1, 4.0)]
clips: 24 ; fades at 0(+4.0) 1(-2.0) 2(-2.0) 4(+2.0) 6(-2.0) 13(+2.0) 15(+4.0) 22(-8.0) ; clip 22 offset 2.0 length 26.0
STRUCTURE-OK   (asserts: names, mute-only-on-Perc, no solos, all length_beats set, 24 clips, trim on 22)
```

Standard gates:
```
$ .builds/wp0197/op_tests                         → ALL PASS (0 failures)                 exit 0
$ bash harness/run_goldens.sh                     → run_goldens: 16 fixtures, 0 failures  exit 0   (no golden added/changed)
$ bash harness/test_cli_errors.sh                 → test_cli_errors: 0 failures           exit 0
$ UV_CACHE_DIR=… bash harness/test_glue_scripts.sh → test_glue_scripts: 0 failures        exit 0
$ DZ_BUILD_TYPE=Debug bash dz_build.sh --sanitize --target op_tests --out .builds/wp0197-san-debug → exit 0
$ .builds/wp0197-san-debug/op_tests               → ALL PASS (0 failures)                 exit 0
```

### Metrics
- Golden: 8640000 frames (180.000 s @ 48 kHz), peak −0.9999 dBFS, −15.6326 LUFS, sha256 `ea32b748…fdca`.
- Un-limited render (same seed): peak 0.0000 dBFS — clips, byte-different from the limited bounce.
- Stem peaks (int16 of 32767): kick 19175, snare 16451, hat 8474, bass 10415, perc 0 (muted → all-zero).
- Intro window (first 100 ms): mix peak 36 vs no-fade twin 8474 → ratio ~235× (contract asks ≥ 8×).
- Outro window (last 1 s): mix peak 0 (< 20), untrimmed twin 8474 (> 2000).

### PROPOSE: notes (gaps found — NOT fixed here, per package rules)
1. **One drum hit** (drafting gap, confirmed in practice): Snare/Hat/Perc are the single
   `kits/kick.wav` re-voiced by `set_sampler_trim`/`set_sampler_root`. A follow-up package adding a small
   CC0 kit (kick/snare/closed-hat/clap) under `tests/fixtures/goldens/kits/` with recorded licences would
   make the exit demo honest to its name.
2. **CI wiring**: nothing runs `make_phase1_song.sh --check` automatically; PROPOSE one line next to the
   goldens step in `.github/workflows/ci.yml` (this package leaves `.github/` untouched).
3. **No arrangement-level mute/solo**: per-track only, so "drop the Perc layer for the break" is impossible;
   the song keeps Perc muted for the whole take. PROPOSE discussing a clip-scoped mute op before building it.
4. **Sanitizer build type on this rig**: `dz_build.sh --sanitize` defaults to `CMAKE_BUILD_TYPE=Release`, and
   on gcc 13 / aarch64 the sanitizer build then dies on a false-positive
   `-Werror=array-bounds` for the pre-existing `out.arrangement = {clip};` in
   `src/engine/render.cpp:786` (inlined `vector::operator=(initializer_list)` memmove analysis). Every
   recent green sanitizers run in `build-san-*` dirs uses Debug — PROPOSE either documenting
   `DZ_BUILD_TYPE=Debug` for the sanitizers job or fixing `pattern_mode_project` to assign via
   `out.arrangement.clear(); out.arrangement.push_back(clip);` in a dedicated package (I ran the gate as
   `DZ_BUILD_TYPE=Debug … --sanitize`, matching the established local convention; it is green).
