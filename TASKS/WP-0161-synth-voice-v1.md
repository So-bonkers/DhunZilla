---
id: WP-0161
title: Synth voice v1 — waveform (sine/saw/square), ADSR, low-pass filter, `set_synth_params` op
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.5
human_review: none
---

## Context — why this exists
The prototype goal is "arrange a 3-minute beat with samples and one synth". Today's `synth` voice has one
fixed timbre (fundamental + octave partial) and a linear attack/release. This package adds a small, fully
deterministic subtractive synth as **optional** voice parameters: oscillator `waveform`, an `adsr` envelope
with a release tail, and a biquad low-pass (`cutoff_hz`, `resonance`) — plus one operation,
`set_synth_params`, so the GUI/copilot can edit them. With none of the new keys present the synth must render
**bit-identically** to today (all existing goldens unchanged).

## Spec — normative and already installed in this branch
`docs/spec/project-model.md` §"Synth voice v1 — additive (WP-0161)" and `docs/spec/operations.md`
§"Synth parameters operation — additive (WP-0161)" are in the repo. **Read both fully; they are exact and
normative (formulas, double-vs-float, evaluation order, error strings). Do not edit `docs/spec/`.** Implement
the formulas literally; a hidden check compares rendered samples against an independent implementation of
those exact formulas to a tight tolerance, so do not "improve" them (no oversampling, no smoothing, no
different PolyBLEP variant, no float shortcuts in the v1 path). Summary only:
- Voice keys on `kind:"synth"`: `waveform` (`sine|saw|square`, PolyBLEP saw/square, amplitude 0.5),
  `adsr` object (`attack_s`, `decay_s`, `sustain`, `release_s`; release tail after note-off),
  `cutoff_hz` + `resonance` (RBJ biquad low-pass, double, direct form I).
- Absent keys = legacy path, kept **literally unchanged** (guard it with one `if` and leave the old loop alone).
- Op `set_synth_params {index, waveform?, adsr?, cutoff_hz?, resonance?}`; `null` resets a key.

## Interface (exact names)
- `src/engine/model.hpp`: `enum class Waveform { Legacy, Sine, Saw, Square };` and new `Voice` members
  `Waveform waveform = Waveform::Legacy; bool has_adsr = false; double attack_s = 0.005, decay_s = 0.0,
  sustain = 1.0, release_s = 0.030; double cutoff_hz = 0.0; double resonance = 0.7071;`
  (`cutoff_hz == 0.0` means "no filter"; the parser only accepts 20..20000 so 0 never comes from a file).
- `src/engine/model.cpp`: parse + validate (exact messages from the spec) in `parse_voice`; canonical save
  per the spec (all four `adsr` keys when present; `resonance` only when `!= 0.7071`).
- `src/engine/render.cpp`: new v1 render path taken only when `waveform != Legacy || has_adsr || cutoff_hz > 0`.
  Use only `dsp::sind_`, `dsp::cosd_`, `dsp::powd_` from `src/engine/dsp.hpp` (all exist) for transcendental
  functions; `std::floor`/`std::ceil`/`std::min` are fine. No fast-math, no randomness, no clock.
- `src/engine/ops.cpp`: new `set_synth_params` branch next to `set_sampler_trim` (same pattern: strict keys,
  JSON edit on the canonical copy, model parse enforces ranges — no second ruleset).

## Acceptance criteria
- [ ] Everything in the two spec sections implemented; error strings character-exact.
- [ ] **All existing goldens unchanged** — `bash harness/run_goldens.sh` green with no fixture edited
      (`pitched` and `imported` use the synth voice and prove the legacy path is untouched).
- [ ] New golden, authorized as the ONLY fixture addition: create `tests/fixtures/goldens/synth_v1.opj` with
      **exactly** this content (byte for byte), then create `tests/fixtures/goldens/synth_v1.wav` by running
      `build/opd render tests/fixtures/goldens/synth_v1.opj -o tests/fixtures/goldens/synth_v1.wav`
      (the golden harness picks it up automatically):
```json
{
  "format_version": 0,
  "bpm": 120.0,
  "sample_rate": 48000,
  "seed": 161,
  "length_bars": 2,
  "beats_per_bar": 4,
  "tracks": [
    { "name": "SawLead", "gain_db": -10.0, "pan": -0.3,
      "voice": { "kind": "synth", "waveform": "saw",
                 "adsr": { "attack_s": 0.01, "decay_s": 0.15, "sustain": 0.5, "release_s": 0.2 },
                 "cutoff_hz": 2400.0, "resonance": 2.5 } },
    { "name": "SquareBass", "gain_db": -9.0, "pan": 0.0,
      "voice": { "kind": "synth", "waveform": "square",
                 "adsr": { "attack_s": 0.002, "decay_s": 0.1, "sustain": 0.6, "release_s": 0.08 },
                 "cutoff_hz": 500.0 } },
    { "name": "SineBell", "gain_db": -12.0, "pan": 0.4,
      "voice": { "kind": "synth", "waveform": "sine",
                 "adsr": { "attack_s": 0.0, "decay_s": 0.6, "sustain": 0.0, "release_s": 0.05 } } },
    { "name": "OldSynth", "gain_db": -14.0, "pan": 0.0,
      "voice": { "kind": "synth", "attack_ms": 5.0, "release_ms": 60.0 } }
  ],
  "patterns": [
    { "track": 0, "notes": [
      { "t": 0.0, "dur": 0.75, "note": 72 }, { "t": 1.0, "dur": 0.75, "note": 76 },
      { "t": 2.0, "dur": 0.75, "note": 79 }, { "t": 3.0, "dur": 1.5, "note": 76 },
      { "t": 4.5, "dur": 0.75, "note": 74 }, { "t": 5.5, "dur": 0.5, "note": 77 },
      { "t": 6.0, "dur": 2.0, "note": 72 } ] },
    { "track": 1, "notes": [
      { "t": 0.0, "dur": 1.0, "note": 36 }, { "t": 2.0, "dur": 1.0, "note": 36 },
      { "t": 4.0, "dur": 1.0, "note": 43 }, { "t": 6.0, "dur": 1.0, "note": 41 } ] },
    { "track": 2, "notes": [
      { "t": 0.5, "dur": 0.25, "note": 84 }, { "t": 2.5, "dur": 0.25, "note": 88 },
      { "t": 4.5, "dur": 0.25, "note": 91 }, { "t": 6.5, "dur": 0.25, "note": 96 } ] },
    { "track": 3, "notes": [
      { "t": 0.0, "dur": 2.0, "note": 55 }, { "t": 4.0, "dur": 2.0, "note": 55 } ] }
  ]
}
```
- [ ] Unit tests in `tests/test_main.cpp` (new `test_synth_v1()` called from `main`), at least 6 new `CHECK`s
      covering: parse of each new key; defaults (`adsr:{}` → 0.005/0/1/0.03); canonical save (adsr all four keys,
      default resonance omitted, legacy synth voice bytes unchanged); validation messages for at least four
      different error cases; release tail present after note-off and silent after `len + ceil(release_s*sr)`;
      `set_synth_params` set + `null` reset + rejection (`track is not a synth voice`). Any temp files must live
      under `std::filesystem::temp_directory_path()` in a per-test directory name that includes the process id.
- [ ] `opd render` of a project using the new keys exits 0 and is bit-identical run to run; invalid values exit 4
      with `error: parse: <spec message>` (existing parse-error plumbing — no CLI changes needed).
- [ ] Gates: `cmake --build build -j4`, `build/op_tests`, `harness/run_goldens.sh`, `harness/test_cli_errors.sh`,
      `harness/test_glue_scripts.sh` all green; **0 compiler warnings** (`-Werror` is on); sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` passes.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), existing golden fixtures, `docs/spec/` (already
  provided), `docs/adr/`, `src/app/`, `src/doc/`, `src/cli/`, `ai-sidecar/`.
- `opd diff` support for the new op (a synth-params difference stays `unsupported difference: tracks[i].voice`),
  voice stealing / monophonic modes, filter envelope/LFO, unison, new CLI flags, MIDI import changes.

## Git
- Branch `wp-0161-synth-voice-v1` from `main`. Commit: `WP-0161: synth voice v1 (waveform, ADSR, low-pass, set_synth_params)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` — added `enum class Waveform { Legacy, Sine, Saw, Square };` and the new
    `Voice` members exactly as specified (`waveform`, `has_adsr`, `attack_s=0.005`, `decay_s=0.0`,
    `sustain=1.0`, `release_s=0.030`, `cutoff_hz=0.0`, `resonance=0.7071`).
  - `src/engine/model.cpp` — `parse_voice`: synth-v1 validation block (spec order: non-synth-key →
    waveform string → adsr object/unknown-key/four inner ranges in order → cutoff_hz → resonance;
    first failure wins, character-exact messages); `voice_to_json`: canonical save per spec
    (`waveform` only when set; `adsr` only when present, then all four inner keys; `cutoff_hz` only
    when set; `resonance` only when `!= 0.7071`; legacy synth voice bytes untouched).
  - `src/engine/render.cpp` — new `render_synth_v1()` implementing the spec formulas literally:
    double throughout (`dsp::sind_/cosd_/powd_` only for transcendentals), pitch guard
    (`f<=0 || dt>=0.5` → nothing, waveform-set only), PolyBLEP sine/saw/square at amplitude 0.5
    (`ph = f*n/sr` multiply-first), RBJ biquad LPF direct-form-I in double with per-note zero state
    (`fc=min(cutoff_hz, 0.45·sr)`), ADSR `g(t)` + release tail `tail=ceil(R·sr)` from
    `e_off=g(len/sr)`, legacy float osc/envelope expressions widened to double when the respective
    key is absent. Dispatched behind one `if` in the switch (`waveform != Legacy || has_adsr ||
    cutoff_hz > 0`); the legacy loop is byte-for-byte unchanged.
  - `src/engine/ops.cpp` — new `set_synth_params` branch next to `set_sampler_trim`, same pattern:
    strict keys → `arg_index` → `no synth parameters given` → `track is not a synth voice` → JSON
    write (null removes key; adsr replaces whole object) → model parse enforces ranges.
  - `tests/test_main.cpp` — new `test_synth_v1()` called from `main`, **33 CHECKs** covering parse of
    every key, enum mapping, `adsr:{}` defaults, ten exact validation messages (incl. first-present-key
    order on non-synth voices), canonical save (all four adsr keys / default resonance omitted /
    legacy voice bytes unchanged / roundtrip stable), file save-load under a pid-named temp dir in
    `std::filesystem::temp_directory_path()`, release tail present after note-off + silent from
    `len+ceil(release_s·sr)` on, repeat-render bit-exactness, e_off=0 hard silence, and
    set_synth_params set / null-reset byte-identical / five rejection strings.
  - New golden (the only fixture addition): `tests/fixtures/goldens/synth_v1.opj` created **byte for
    byte** from the task-file block (`sed -n '54,91p' TASKS/WP-0161-synth-voice-v1.md`, verified by
    `diff` → identical) and `synth_v1.wav` rendered with the exact command below.
  - No existing fixture touched; no CLI/spec/ADR changes.
- **How verified:** (all commands run on branch `wp-0161-synth-voice-v1`)
  - Golden fixture creation + render:
    ```
    $ sed -n '54,91p' TASKS/WP-0161-synth-voice-v1.md > tests/fixtures/goldens/synth_v1.opj
    $ diff <(sed -n '54,91p' TASKS/WP-0161-synth-voice-v1.md) tests/fixtures/goldens/synth_v1.opj   # (no output = byte-exact)
    $ build/opd render tests/fixtures/goldens/synth_v1.opj -o tests/fixtures/goldens/synth_v1.wav
    rendered 192000 frames @48000 Hz -> tests/fixtures/goldens/synth_v1.wav (seed=161)
    ```
  - `cmake --build build -j4` — clean; from-scratch reconfigure (`rm -rf build-clean && cmake -S . -B build-clean && cmake --build build-clean -j4`) → `grep -icE "warning|error"` on the full log = **0** (`-Werror` is ON):
    ```
    0
    ---
    op_tests_exit=0
    ```
  - `build/op_tests` — all pass, incl. the 33 new checks:
    ```
    PASS synth_v1.parse_all_keys … PASS synth_v1.op_reject_cutoff_range
    INFO synth v1: 4800-frame release tail verified after note-off
    ALL PASS (0 failures)        # exit code 0; $ ./build/op_tests | grep -c "PASS synth_v1" → 33
    ```
  - `bash harness/run_goldens.sh` — **all existing goldens unchanged**, new one added:
    ```
    PASS arranged: deterministic + golden match
    PASS demo: deterministic + golden match
    PASS imported: deterministic + golden match
    PASS mixer: deterministic + golden match
    PASS panorama: deterministic + golden match
    PASS pitched: deterministic + golden match          # synth voice → legacy path untouched
    PASS resampled: deterministic + golden match
    PASS sampler_trim: deterministic + golden match
    PASS synth_v1: deterministic + golden match         # new fixture
    PASS tempo: deterministic + golden match
    PASS negative-control (corrupt golden rejected)
    run_goldens: 10 fixtures, 0 failures
    ```
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  - `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh` →
    `test_glue_scripts: 0 failures`.
  - Sanitizers (`cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4 && ./build-san/op_tests`):
    ```
    DZ_SANITIZE:BOOL=ON
    [100%] Built target op_tests
    INFO synth v1: 4800-frame release tail verified after note-off
    ALL PASS (0 failures)
    ```
  - CLI acceptance (render exit 0 + bit-identical; invalid → exit 4 with `error: parse: <spec msg>`):
    ```
    $ build/opd render tests/fixtures/goldens/synth_v1.opj -o r1.wav   # and again → r2.wav
    BIT-IDENTICAL run-to-run (exit 0/0)
    $ build/opd render bad1.opj …   exit=4   error: parse: voice.waveform must be sine|saw|square
    $ build/opd render bad2.opj …   exit=4   error: parse: voice.adsr.attack_s must be a number in [0, 10]
    $ build/opd render bad3.opj …   exit=4   error: parse: voice.cutoff_hz must be a number in [20, 20000]
    ```
  - Op through the CLI journal path + `opd diff` unchanged (demo artifact = committed golden
    `synth_v1.wav`, deterministic; legacy tracks in the same project prove coexistence):
    ```
    $ build/opd apply base.opj set.opjl -o edited.opj && … reset journal … && cmp base.opj back.opj
    NULL-RESET BYTE-IDENTICAL (canonical baseline)
    $ build/opd diff a2.opj b.opj -o edits.opjl
    exit=5  error: diff: unsupported difference: tracks[0].voice     # matches the pinned WP-0146 contract
    ```
  - Acceptance criteria: all checked — spec implemented with exact error strings; existing goldens
    green with no fixture edited (`git status` shows only the two authorized new fixture files);
    33 unit CHECKs (≥6 required) covering every listed area; CLI exit codes verified; all gates +
    sanitizer build green.
- **Metrics:** engine diff ≈ +215 lines across 4 files, all additive; render v1 path adds one branch
  to the voice switch; golden project renders 192000 frames (4 bars @120bpm) with 3 v1 tracks + 1
  legacy track.
- **PROPOSE / BLOCKED:** nothing blocked. `PROPOSE:` none — no out-of-scope changes were needed.
  (Note for the shipper: ROADMAP row intentionally not added, per task Out of scope.)
