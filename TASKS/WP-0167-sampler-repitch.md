---
id: WP-0167
title: Sampler repitch — optional per-voice root_note, notes play the sample at 2^((P-root)/12) + set_sampler_root op
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.5
human_review: none
---

## Context — why this exists
Phase 1's exit criterion is "arrange a 3-minute beat with samples and one synth". Today a sampler plays every
note at the asset's original pitch, so a one-shot bass or pluck sample cannot play a melody. This WP adds an
optional per-voice `root_note` (the MIDI pitch the sample sounds at): a note with pitch `P` then plays the
sample at playback ratio `2^((P - root_note)/12)` using plain **linear interpolation** with an exactly
specified read position. Without `root_note` nothing changes **bit-exactly**; no existing golden may change.

## Spec — normative
The two spec files `docs/spec/project-model.md` (new section "Sampler repitch — additive (WP-0167)") and
`docs/spec/operations.md` (new section "Sampler repitch operation — additive (WP-0167)") are **already updated
by the runner** before you start. Read both new sections first: they are the contract (ratio table, read
position, interpolation formula, end-of-source rule, validation order, error strings, op argument rules). Do
not edit those two spec files. Summary (the spec sections win on any difference):

- Voice JSON key `root_note` (integer 0..127, sampler voices only, default absent). Note pitch is the existing
  `note.note` (`Note::midi_note`, `-1` = absent). Absent root, absent note pitch, or `P == root_note` →
  **the existing code path, unchanged** (shift 0).
- Stage order: resample to project rate (WP-0138) → trim slice (WP-0156) → repitch. `x[j] = buffer[i0 + j]`,
  `L = i1 - i0`.
- `shift = P - root`; `q = floor(shift/12)`, `r = shift - 12*q`; `ratio = T[r] * 2^q` in double, with the 12
  constants `T` from the spec and `2^q` applied by exact multiplication/division by `2.0` in a loop (no `pow`,
  no libm). `+12` → exactly 2.0, `-12` → exactly 0.5.
- Per output frame `n`: `pos = double(n) * ratio` (multiplication, not a running sum); `k = floor(pos)` as
  `long long`; `frac = pos - double(k)`; stop (note ends) at the first `n` with `k + 1 >= L`; else per channel
  `y = double(x[k]) + frac * (double(x[k+1]) - double(x[k]))`, mixed as `float(y)` via the existing
  `add_stereo`. Total frames = `min(frames before source runs out, llround(note.dur * spb))`. `L == 1` → silence.
- Parse errors (exact): `voice.root_note is only valid for sampler voices`,
  `voice.root_note must be an integer 0..127` (checked after the WP-0156 trim-key checks).
- Canonical save emits `root_note` only when present (after `end_s`).
- Op `set_sampler_root {index, root_note}` (`root_note` integer or `null` = clear), strings per the spec section.

## Implementation guide (files, 5 interface files)
1. `src/engine/model.hpp` — add to `struct Voice`: `int root_note = -1;  // sampler: -1 = absent` (exactly this name).
2. `src/engine/model.cpp` — in `parse_voice` after the WP-0156 trim block: if `j.contains("root_note")`: non-sampler
   → error; else require `is_number_integer()` and range 0..127 (a `null`, string, or float like `60.0` is the
   range error), set `v.root_note`. In `voice_to_json` (Sampler case) emit `root_note` after `end_s` when `>= 0`.
   Untouched voices must save byte-identically.
3. `src/engine/render.cpp` — in `render_sampler`, after the slice `i0`/`i1` is computed: if
   `v.root_note < 0 || note.midi_note < 0 || note.midi_note == v.root_note` run the existing loop untouched
   (do not restructure it; the default path must stay the existing code). Otherwise compute `ratio` and run the
   new interpolating loop. Put the ratio in a small file-local `static double sampler_ratio(int shift)` with the
   12-entry `constexpr double` table. Use `std::floor` only via the integer conversion shown in the spec
   (`pos >= 0`, so `static_cast<long long>(pos)` IS the floor; no libm needed).
4. `src/engine/ops.cpp` — add the `set_sampler_root` branch in `apply_named` right after `set_sampler_trim`
   (same style: `check_keys`, `arg_index`; `root_note` must be present and `is_number_integer()` or `is_null()`
   else `'root_note' must be an integer or null`; non-sampler → `track is not a sampler voice`; `null` erases the
   key). Do NOT re-implement the range check: the model parse enforces it. Do not touch `diff_projects`.
5. `tests/test_main.cpp` — new `static void test_sampler_repitch()` called from `main`, **≥ 10 new CHECKs**,
   using a unique temp dir (`std::filesystem::temp_directory_path() / ("op_repitch_" + std::to_string(getpid()))`,
   removed at the end) for generated assets: a synthetic asset whose frame `i` holds `i/32768.0f`
   (PCM16-exact ramp); +12 gives `out[n] == x[2n]`, -12 gives interpolated midpoints, +7 matches your own
   in-test recomputation from the spec formula within 1 LSB, the note ends when the source runs out, a short note
   gates, shift 0 / absent pitch / absent root render byte-identical to the no-root project, mono vs stereo
   asset, trimmed slice is repitched (slice stage before repitch), each parse error string, canonical save omits
   a missing root and emits a present one, op sets / clears (`null`) / rejects.

## New golden (authorized: add exactly these four files, nothing else under `tests/fixtures/`)
1. `harness/make_repitch_golden.py` — pure-stdlib, integer-only generator for the asset below (model it on
   `harness/make_trim_golden.py`; header docstring says how to regenerate).
2. `tests/fixtures/goldens/kits/saw48k.wav` — mono PCM16 **48000 Hz**, 9600 frames, frame `i` = int16 value
   `(i % 100) * 200 - 9900` (a 480 Hz sawtooth, exact integers). Canonical 44-byte header (RIFF/WAVE/fmt/data).
3. `tests/fixtures/goldens/sampler_repitch.opj` — canonical JSON (alphabetically sorted keys, 2-space indent, as
   the other goldens): `beats_per_bar 4, bpm 120.0, format_version 0, length_bars 1.0, sample_rate 48000, seed 7`;
   one track `{"gain_db": 0.0, "name": "SawMelody", "pan": 0.0, "voice": {"kind": "sampler",
   "path": "kits/saw48k.wav", "root_note": 60}}`; one pattern for track 0 with six notes, each `{"dur", "note", "t"}`
   except the last (no `note`): `(t 0.0, note 60, dur 0.5)`, `(0.5, 64, 0.5)`, `(1.0, 67, 0.5)`, `(1.5, 72, 0.5)`,
   `(2.0, 55, 0.5)`, `(2.5, none, 0.25)`. (A C-major arpeggio, a low G, then an unshifted gated hit.)
4. `tests/fixtures/goldens/sampler_repitch.wav` — produced by `build/opd render` of (3). `harness/run_goldens.sh`
   discovers goldens by glob; do not edit it. Never modify any existing fixture.

## Acceptance criteria
- [x] Defaults bit-exact: all 9 pre-existing goldens + `sampler_repitch` pass `bash harness/run_goldens.sh`.
- [x] `build/opd render tests/fixtures/goldens/sampler_repitch.opj`: left channel, note at beat 1.5 (frame 36000,
      pitch +12): output frame `n` equals source `x[2n]` (× equal-power pan 0.7071…) for `n < 4800`, then
      silence; note at beat 0 reproduces the asset (9600 frames) then silence.
- [x] `opd apply` with `{"op":"set_sampler_root","index":0,"root_note":60}` works; `null` clears; rejections leave
      the file unwritten and exit 5 with the spec'd strings.
- [x] Unit tests (≥ 10 new CHECKs, all pass); `build/op_tests` prints `ALL PASS`.
- [x] `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh` green; build has 0 warnings
      (`-Werror` default); sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` passes. Build with `-j4` max.
- [x] ADR-0004 respected: no clock/randomness, no libm for the ratio, `double` arithmetic only as specified.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), `docs/spec/*` (already updated by the runner),
  existing fixtures, `harness/run_goldens.sh`, `src/cli/`, `src/doc/`, `src/app/`, `ai-sidecar/`, `docs/adr/`,
  the resampler itself, `diff_projects`, MIDI import/export.
- Looping, per-note root, pitch bend/glide, fine-tune cents, formant preservation, higher-order interpolation, UI.

## Git
- Branch `wp-0167-sampler-repitch` from `main`. Commit: `WP-0167: sampler repitch (root_note) + set_sampler_root op`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` — `Voice::root_note` (int, `-1` = absent) added exactly as specified.
  - `src/engine/model.cpp` — `parse_voice`: after the WP-0156 trim block, a present `root_note` must be on a
    sampler voice (`voice.root_note is only valid for sampler voices`) and a JSON integer in `0..127`, else
    `voice.root_note must be an integer 0..127` (string / float / null / `-1` / `128` all give that string; no
    overflow: nlohmann keeps out-of-int64 literals as floats, which fail `is_number_integer()`).
    `voice_to_json`: emits `root_note` after `end_s` only when `>= 0`.
  - `src/engine/render.cpp` — file-local `static double sampler_ratio(int shift)` with the spec's 12-entry
    `constexpr double` table, floor division and `2^q` by exact doubling/halving (no libm); in `render_sampler`,
    after the slice `[i0,i1)`: only `root_note >= 0 && note.midi_note >= 0 && midi_note != root_note` takes the
    new loop (`pos = double(n)*ratio`, `k` by truncation, break at `k+1 >= slice`, double linear interpolation
    per channel → `float(y)` → existing `add_stereo`). The default path is the untouched pre-WP-0167 loop.
  - `src/engine/ops.cpp` — `set_sampler_root` branch right after `set_sampler_trim`: `check_keys`, `arg_index`,
    `missing argument 'root_note'`, `'root_note' must be an integer or null`, `track is not a sampler voice`,
    `null` erases the key; range left to the model parse. `diff_projects` untouched.
  - `tests/test_main.cpp` — `test_sampler_repitch()` (called from `main`), **46 new CHECKs**, assets generated in
    a unique temp dir (`temp_directory_path()/("op_repitch_"+getpid())`, removed at the end): +12 doubling,
    −12 exact midpoints, +7 vs an independent in-test recomputation of the spec formula (≤ 1 LSB), source
    run-out silence, short-note gating, shift-0 / absent-pitch / P==root byte-identical to the unrooted render,
    stereo asset per channel, slice-before-repitch, `L == 1` silence, all parse-error strings + trim-first order,
    canonical save omit/emit/round-trip, op set/clear/reject (7 cases).
  - New fixtures (only these four added under `tests/fixtures/`, nothing existing modified):
    `harness/make_repitch_golden.py` (pure stdlib, integer-only), `kits/saw48k.wav`
    (sha256 `683e7de812f0b5b93c4bba6b956d1784a14af84299db1768238b3c9ee7e65b1e`, 9600 frames mono PCM16 @48 kHz),
    `sampler_repitch.opj` (`f90815e8ea7ed9e438be889edfd4c49c58264dab538c8780011a1f870ed61e36`),
    `sampler_repitch.wav` (`bded5e9bbedb22afe52c1d775c8b28ca6eb4f2414f02d148ed8ff0949a315611`).
  - `docs/spec/project-model.md`, `docs/spec/operations.md`: the runner-installed sections, committed unchanged.
- **How verified:** (all commands run in `/home/shubhankar_dgx/qwen-scratch/openpattern`)
  - Clean build, `-Werror` default, `-j4`:
    ```
    $ rm -rf build && cmake -S . -B build >/dev/null && cmake --build build -j4   # BUILD_EXIT=0
    $ grep -ciE "warning|error" /tmp/build_full.log
    0 warning/error lines
    ```
  - Unit tests (`grep -c` of the run: 733 PASS lines total, **46** from this WP, 0 FAIL):
    ```
    $ ./build/op_tests | tail -2
    INFO sampler repitch: +12 doubling, -12 midpoints, +7 spec recomputation checked
    ALL PASS (0 failures)          # exit 0
    $ ./build/op_tests | grep -cE "^PASS (repitch\.|ops\.set_sampler_root)"
    46
    ```
  - Goldens — the 9 pre-existing ones still match bit-exactly, plus the new one:
    ```
    $ bash harness/run_goldens.sh | tail -3
    PASS sampler_trim: deterministic + golden match      (…arranged/demo/imported/mixer/panorama/pitched/
                                                          resampled/tempo all PASS)
    PASS negative-control (corrupt golden rejected)
    run_goldens: 10 fixtures, 0 failures                 # exit 0
    ```
  - Acceptance render check on `sampler_repitch.opj` (ad-hoc stdlib script over the rendered WAV):
    ```
    $ ./build/opd render tests/fixtures/goldens/sampler_repitch.opj -o tests/fixtures/goldens/sampler_repitch.wav
    rendered 96000 frames @48000 Hz -> tests/fixtures/goldens/sampler_repitch.wav (seed=7)
    $ python3 /tmp/wp167_check.py
    golden: 96000 frames @48000Hz stereo PCM16; asset 9600 frames mono PCM16
    PASS +12 note (beat 1.5, frame 36000): left[n] == x[2n]*pan(0.7071..) for n<4800 (first mismatch None)
    PASS +12 note silent from frame 40800 to next note at 48000
    PASS beat-0 note (pitch == root) reproduces the asset for all 9600 frames (first mismatch None)
    PASS asset silent after frame 9600 until next note at 12000
    MEASURED +12 note: first sounding frame n=0 -> L=-7000 (x[0]=-9900, x[0]*g=-7000.36)
    MEASURED +12 note: last  sounding frame n=4799 -> L=6859 (x[9598]=9700, x[9598]*g=6858.94)
    MEASURED +12 note: first silent frame n=4800 -> L=0
    PASS EXACT int16: +12 render frame n == shift-0 render frame 2n
    acceptance: OK                                       # exit 0
    ```
    (The last line is the strongest form of the `out[n] == x[2n]` criterion: a single-note +12 render and a
    shift-0 render of the same asset/gain are int16-equal under the read `n → 2n`, so the doubling holds with
    zero tolerance once the shared pan gain is factored out.)
  - `opd apply` contract (set / clear / reject; rejections exit 5, output file never created):
    ```
    $ opd apply p.opj <{"op":"set_sampler_root","index":0,"root_note":60}> -o out_set.opj   → exit 0, "root_note": 60 in file
    $ opd apply out_set.opj <{"op":"set_sampler_root","index":0,"root_note":null}> -o out_clear.opj
        → exit 0; grep -c '"root_note"' out_clear.opj → 0 (voice saved as {kind, path} only)
    $ opd apply p.opj <…128> -o never1.opj ; echo $? stderr
        exit=5 stderr=[error: apply: line 1: set_sampler_root: voice.root_note must be an integer 0..127]  → OUTPUT_NOT_CREATED
    $ opd apply p.opj <…60.5> -o never2.opj
        exit=5 stderr=[error: apply: line 1: set_sampler_root: 'root_note' must be an integer or null]      → OUTPUT_NOT_CREATED
    $ opd apply p.opj <index 9> -o never3.opj
        exit=5 stderr=[error: apply: line 1: set_sampler_root: 'index' out of range]
    $ opd apply … -o keep.opj (pre-existing output) → exit 5, KEEP_UNMODIFIED (cmp with previous bytes)
    ```
  - `opd diff` unchanged, as the spec requires: rooted vs unrooted project →
    `error: diff: unsupported difference: tracks[0].voice` (exit 5).
  - Canonical form of the golden `.opj` proven by a round-trip through the engine's own save path:
    `$ opd apply sampler_repitch.opj <{"op":"set_bpm","bpm":120.0}> -o /tmp/canon.opj && cmp …` → `CANONICAL_MATCH`.
  - Generator reproducibility (byte-for-byte): re-running `python3 harness/make_repitch_golden.py` leaves the
    committed `kits/saw48k.wav` at sha256 `683e7de8…65b1e` and git reports no change.
  - CLI + glue e2e (`UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv` for the uv-based glue tests):
    ```
    $ bash harness/test_cli_errors.sh   → test_cli_errors: 0 failures            (exit 0)
    $ bash harness/test_glue_scripts.sh → test_glue_scripts: 0 failures          (exit 0)
    ```
  - Sanitizers (`-DDZ_SANITIZE=ON`, `-j4`): `cmake --build build-san -j4 --target op_tests` (no warnings) then
    `./build-san/op_tests` → `ALL PASS (0 failures)`, exit 0, no ASAN/UBSAN report.
  - Sidecar untouched by this WP but re-checked green anyway: `uv run ruff check .` → `All checks passed!`;
    `uv run pytest -q` → `109 passed, 2 skipped`.
  - Demo artifact / audio impact: output changes only for projects that use the new key. The committed
    `tests/fixtures/goldens/sampler_repitch.wav` (a C-major arpeggio + low G + unshifted gated hit) is the demo;
    the 9 pre-existing goldens still match bit-exactly, which is the before/after evidence that nothing else moved.
- **Metrics:** engine diff +71 lines across 4 files (+289 test lines); ratio table 12 doubles; render loop adds one
  multiply + one truncation + one compare per output frame on the repitch path only; `+12` → ratio exactly 2.0,
  `-12` → exactly 0.5 (asserted via the doubling/midpoint checks).
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE (out of scope here, not done): `opd info <project.opj>` prints
  tracks but no voice details, so a configured `root_note` is invisible there — a one-line addition in `src/cli/`
  could show `sampler root=<n>`; left for a future package since `src/cli/` is out of scope for WP-0167.
