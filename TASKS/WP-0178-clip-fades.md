---
id: WP-0178
title: Clip fades — optional fade_in_beats / fade_out_beats on arrangement clips, linear gain ramp at render time, set_clip_fades op + undo + opd diff, new golden
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [model, ops, render, tests:main, spec:project-model, spec:operations,
          new:tests/fixtures/goldens/clip_fades.opj, new:tests/fixtures/goldens/clip_fades.wav]
---

## Context — why this exists

Clips already carry a static level (`gain_db`, WP-0120) but cannot start or end softly: every cut is an
instant jump, so an arranged beat has hard edges at section boundaries and a sampled loop that ends mid-wave
clicks. This package adds the smallest honest version of a fade — **two optional per-clip numbers**
`fade_in_beats` / `fade_out_beats`, a **linear gain ramp applied to that clip's contribution at render time**
(nothing in the note data changes), one operation `set_clip_fades` (undo/redo for free, it is a model edit) and
`opd diff` support, plus one new golden fixture. Fades absent ⇒ bit-identical renders: every existing golden must
still pass unchanged.

## Spec — normative

`docs/spec/project-model.md` (new section **"Clip fades — additive (WP-0178)"**) and `docs/spec/operations.md`
(new section **"Clip fade operation — additive (WP-0178)"** plus an amendment to §`opd diff` step 9) are
**already updated by the runner** before you start. Read them first; they are the contract (region bounds, exact
gain formulas, overlap rule, validation order and strings, canonical-save rules, diff order). Do not edit those
two spec files. Summary (the spec sections win on any difference):

- Clip JSON keys `fade_in_beats`, `fade_out_beats`: numbers in `[0, 4096]` (beats), default `0` = off.
- Parse errors (exact, one per field): `clip.fade_in_beats must be a number in [0, 4096]` /
  `clip.fade_out_beats must be a number in [0, 4096]` (non-number — including `null`, strings, booleans —
  non-finite, negative or `> 4096`). `-0.0` is accepted and equals `0`.
- Canonical save omits each key when it is exactly `0`; keys inside a clip object dump alphabetically, so both
  land before `gain_db`.
- Region bounds use the renderer's own `TempoMap::at()` on beats: with `bpb = beats_per_bar`,
  `base = clip.start_bar * bpb`, `extent = op::pattern_extent(pattern)`, `reps = (extent > 0.0 ? clip.repeats : 1)`,
  `clip_end_beats = base + reps * extent`:
  - fade-in frames `[A, A+N)` with `A = at(base)`, `N = at(base + fade_in_beats) - A`;
  - fade-out frames `[B, B+M)` with `B = at(clip_end_beats - fade_out_beats)`, `M = at(clip_end_beats) - B`;
  - a region of `0` frames has no effect.
- Gain at absolute frame `f`, in this order: fade-in → `g = float(double(f-A)/double(N))`; else fade-out →
  `g = float(1.0 - double(f-B)/double(M))`; else `g = 1.0f`. Ratio computed in `double`, converted to `float`
  once. On overlapping ramps **fade-in wins** (no product, no error).
- The envelope scales **only that clip's contribution**, after track gain/pan, `gain_db` and velocity: the value
  added to the mix buffer is `(voice_sample * mix_target_gain) * g`. Other clips of the same track are untouched.
- Audio of a clip that continues past `clip_end_beats` (a note longer than the extent) is **not gated**: frames
  at/after `B+M` have gain 1 again.
- One envelope per **clip**, spanning all its repeats — not one ramp per repeat.
- Op `set_clip_fades {index, fade_in_beats, fade_out_beats}` (both arguments always required; a JSON `null`
  clears that side to the default `0`). `opd diff` emits it after `set_clip_gain`, **one op** carrying both
  values from b (`0.0` when absent).

## Implementation guide (4 interface files + tests + golden)

1. `src/engine/model.hpp` — in `struct Clip`, after `gain_db`:
   ```cpp
   double fade_in_beats = 0.0;   // linear fade-in over this many beats from the clip start (WP-0178)
   double fade_out_beats = 0.0;  // linear fade-out over this many beats ending at the clip end (WP-0178)
   ```
2. `src/engine/model.cpp` — in the `arrangement` parse loop, right after the `clip.gain_db` check and before
   `p.arrangement.push_back(c)`, parse both keys with the pattern used for `pattern.swing` (`contains` →
   `is_number()` + `std::isfinite` + range, `< 0.0` so `-0.0` passes), fade-in first, each failure returning its
   own exact message. In `project_to_json`, after the `gain_db` line: emit each key only when it is `!= 0.0`.
3. `src/engine/render.cpp` — in the anonymous namespace, before `struct MixTarget`:
   - `struct ClipEnv { long long in_start = 0, in_len = 0, out_start = 0, out_len = 0; };` (absolute frames; a
     `*_len` of 0 means that ramp is off);
   - `inline float clip_env_gain(const ClipEnv& e, long long frame)` implementing the two branches **in spec
     order** (fade-in checked first) and returning `1.0f` otherwise; the ratio is
     `static_cast<float>(static_cast<double>(...) / static_cast<double>(...))`. Plain arithmetic only — no libm,
     so nothing new goes through `dsp.hpp`.
   - add to `MixTarget`: `const ClipEnv* env = nullptr;  // WP-0178: per-clip fade envelope; null = none`;
   - in `add_stereo`, after the frame bounds check: `const float g = t.env ? clip_env_gain(*t.env, frame) : 1.0f;`
     and multiply **after** the mix-target gains: `+= l * t.gain_l * g;` / `+= r * t.gain_r * g;`
     (with `g == 1.0f` this is bit-identical to today — do not reorder the existing factors).
   - give the `render_pattern_at` lambda one more parameter `const ClipEnv* env`; build the per-clip copy when
     `clip_gain_db != 0.0 || env != nullptr`, set `clip_t.env = env`, and pass `nullptr` from the legacy
     (no-arrangement) call site. In the arrangement loop, when either fade is non-zero build one `ClipEnv` for
     the clip from `tmap.at(...)` exactly as in the Spec section (`tmap` is already in scope there) and pass a
     pointer to it for **every** repeat; otherwise pass `nullptr`. When both fades are 0 nothing extra may be
     computed or copied (the default path stays bit-exact).
4. `src/engine/ops.cpp` — in `apply_named`, add the branch **immediately after `set_clip_gain`**:
   `check_keys(op_json, {"index", "fade_in_beats", "fade_out_beats"}, err)`, then `arg_index(op_json, "index",
   n_clips, idx, err)` where `n_clips = j.contains("arrangement") ? j["arrangement"].size() : 0`, then for each
   of the two keys in that order: missing → `missing argument '<key>'`; not number and not null →
   `'<key>' must be a number or null`; write the value into `j["arrangement"][idx]` or `erase` the key for
   `null`. Do **not** range-check here — the model parse (which produces
   `set_clip_fades: clip.fade_in_beats must be a number in [0, 4096]`) does it. In `diff_projects`, step 9's
   per-clip loop, after the `set_clip_gain` push: read both fields of a and b with the existing `field_or(... ,
   kZeroDb)` helper and push **one**
   `{{"op","set_clip_fades"},{"index",i},{"fade_in_beats",<b value>},{"fade_out_beats",<b value>}}` when either
   differs. Nothing else in `diff_projects` changes.
5. `tests/test_main.cpp` — add `static void test_clip_fades()` (call it from `main()`, next to the other clip
   tests) with **≥ 20 new CHECKs**, using the existing pid-scoped helpers `tmp_file(...)` / `tmp_dir()` (never a
   fixed `/tmp/op_*` path). Suggested coverage, all of it already pinned by the spec sections: parse defaults and
   explicit values; one exact-message rejection per bad kind for each field (`-0.5`, `4096.5`, `"0.5"`, `null`,
   and a fade-out-only failure with a valid fade-in); `-0.0` treated as 0; canonical save emits/omits; file
   round-trip; renders of a hard-left (`pan -1.0`, so `gain_l == 1.0f` exactly) sampler project over an asset of
   constant `0.25` compared **bit-exactly** against the spec formula at the ramp frames, across the whole plateau,
   and at the clip end; explicit `0.0/0.0` renders bit-identically to absent keys; a second unfaded clip of the
   same pattern at the same bar is unaffected (per-clip scoping); `repeats 2` gets one ramp at the clip start and
   its fade-out on the last repeat; a tempo map moves the region bounds via `TempoMap::at`; op sets both / null
   clears one side / zeros restore the never-faded canonical bytes; every rejection message above with the project
   left unchanged; `History` undo/redo byte-exact; `diff_projects` emits exactly one `set_clip_fades` (both
   values), after `set_clip_gain`, empty journal when fades are equal, and applying the journal reproduces b.
6. New golden fixture — create **exactly** these two files (touch no existing fixture):
   `tests/fixtures/goldens/clip_fades.opj` with this canonical content (create it by loading an equivalent JSON
   through `opd apply <in.opj> <empty.opjl> -o tests/fixtures/goldens/clip_fades.opj`, so the byte layout is the
   canonical one), then render the wav with the built binary:

   ```json
   {
     "arrangement": [
       { "fade_in_beats": 0.5, "fade_out_beats": 1.0, "pattern": 0, "repeats": 2, "start_bar": 0.0 },
       { "gain_db": -6.0, "pattern": 0, "repeats": 1, "start_bar": 3.0 }
     ],
     "beats_per_bar": 4,
     "bpm": 120.0,
     "format_version": 0,
     "length_bars": 4.0,
     "patterns": [ { "notes": [ { "dur": 0.5, "t": 0.0 }, { "dur": 0.5, "t": 1.0 } ], "track": 0 } ],
     "sample_rate": 48000,
     "seed": 2027,
     "tracks": [ { "gain_db": -3.0, "name": "Kick", "pan": 0.25,
                   "voice": { "kind": "sampler", "path": "kits/kick.wav" } } ]
   }
   ```

   `bash harness/run_goldens.sh` auto-discovers it (it renders twice for determinism and diffs against the .wav):
   ```
   ./build/opd render tests/fixtures/goldens/clip_fades.opj -o tests/fixtures/goldens/clip_fades.wav
   ```
   Do not hand-edit the .wav; regenerate it if your renderer changes.

## Acceptance criteria

- [ ] Defaults bit-exact: all 11 existing goldens still pass `bash harness/run_goldens.sh` unchanged, and a
      project with fades absent renders identically to one with explicit `0.0/0.0`.
- [ ] New golden pair `tests/fixtures/goldens/clip_fades.opj` + `.wav` exists, is the canonical project above,
      and passes `run_goldens.sh` (deterministic double render + golden match). No existing fixture modified.
- [ ] Render semantics: for a hard-left sampler clip over a constant-`0.25` asset at 120 bpm / 48 kHz, every
      frame of the fade-in and fade-out regions equals `0.25f * float(double(u)/double(N))` /
      `0.25f * float(1.0 - double(u)/double(M))` **exactly** (no tolerance needed), the plateau is exactly `0.25f`,
      the clip's first frame is exactly `0.0f`, and audio past `clip_end_beats` keeps gain 1. Same probe under a
      tempo map with bounds from `TempoMap::at`.
- [ ] Fades are per clip, not per repeat, and do not touch other clips of the same track.
- [ ] `opd apply <proj> <journal>` with `{"op":"set_clip_fades","index":0,"fade_in_beats":1.5,"fade_out_beats":0.25}`
      works; `null` clears one side; zeros restore the never-faded canonical bytes; every rejection returns exit 5
      with the exact spec'd string and writes nothing; `History` undo/redo is byte-exact.
- [ ] Bad project values fail `opd render` with exit 4 and the exact message; nothing is written.
- [ ] `opd diff a.opj b.opj -o j.opjl` emits exactly
      `{"fade_in_beats":1.5,"fade_out_beats":0.25,"index":0,"op":"set_clip_fades"}` for a fades-only difference,
      after `set_clip_gain` when both differ, nothing when they are equal; applying the journal reproduces b's bytes.
- [ ] ≥ 20 new CHECKs in `tests/test_main.cpp`; `build/op_tests` ends `ALL PASS`.
- [ ] `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh` green; `-Werror` build has 0 warnings;
      sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` passes. Build with `-j4` max.
- [ ] ADR-0004 respected: no clock, no randomness, no new libm calls (the envelope is `+ - /` and casts only).

## Out of scope — do NOT touch

- `ROADMAP.md` (the elephant adds the row at ship time), existing golden fixtures (`tests/fixtures/goldens/*`
  other than the two new files), `harness/` scripts, `src/app/`, `src/cli/` (no CLI change is needed: the op rides
  on `opd apply`/`opd diff`), `src/doc/`, `ai-sidecar/`, `docs/adr/`, `CMakeLists.txt` (no new target needed),
  `tests/test_loudness.cpp` / `test_normalize.cpp` / `test_lint.cpp` / `test_peaks.cpp`, and `docs/spec/*` other
  than the two files already updated by the runner.
- No changes to `opd analyze`, `opd lint`, `opd opj2mid`, `step_grid` or the app/player: fades are render-only,
  like pattern swing — MIDI export and analysis stay as they are.
- Curved / exponential / equal-power ramps, crossfades between clips, per-note fades, fade handles in a UI, tempo
  maps that would need frame-exact re-derivation beyond `TempoMap::at`, gating a clip's overhanging note tails at
  the fade-out end, and any second op (e.g. `clear_clip_fades` — use `set_clip_fades` with `null`).

## Git

- Branch `wp-0178-clip-fades` from `main`. Commit: `WP-0178: clip fades (linear render-time fade_in_beats/fade_out_beats) + set_clip_fades op + diff + golden`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` — `Clip` gained `double fade_in_beats = 0.0; double fade_out_beats = 0.0;` after `gain_db`.
  - `src/engine/model.cpp` — arrangement parse loop validates both keys (fade-in before fade-out) with the
    swing-style check (`is_number()` + `std::isfinite` + `[0,4096]`, `< 0.0` so `-0.0` passes), each failure
    returning its exact message; `project_to_json` emits each key only when `!= 0.0` (alphabetical dump → both
    land before `gain_db`).
  - `src/engine/render.cpp` — new anon-namespace `struct ClipEnv{in_start,in_len,out_start,out_len}` and
    `clip_env_gain()` (spec order: fade-in first, then fade-out, else `1.0f`; ratio in `double` → one
    `static_cast<float>`; plain `+ - /`, no libm). `MixTarget` gained `const ClipEnv* env = nullptr`.
    `add_stereo` computes `g = t.env ? clip_env_gain(*t.env, frame) : 1.0f` and multiplies **after** the mix-target
    gains (`+= l * t.gain_l * g`). `render_pattern_at` takes a `const ClipEnv* env`; the per-clip copy is built when
    `clip_gain_db != 0 || env != nullptr`; legacy call site passes `nullptr`. The arrangement loop builds one
    `ClipEnv` per clip (bounds via `tmap.at(...)`, span across all repeats) only when a fade is set, else passes
    `nullptr` — the default path computes/copies nothing extra.
  - `src/engine/ops.cpp` — new `set_clip_fades` branch immediately after `set_clip_gain`: strict keys
    `{index,fade_in_beats,fade_out_beats}`, `arg_index` against clip count, per-key in order (missing →
    `missing argument '<key>'`; non-number/non-null → `'<key>' must be a number or null`; write value or `erase`
    for `null`); range left to the model parse. `diff_projects` step 9 pushes ONE `set_clip_fades` carrying both
    b values (`field_or(...,kZeroDb)`) after `set_clip_gain`, only when either side differs.
  - `tests/test_main.cpp` — added `test_clip_fades()` (36 CHECKs) called from `main()` next to `test_clip_gain()`.
  - New golden pair `tests/fixtures/goldens/clip_fades.opj` + `.wav` (canonical project from the task, produced via
    `opd apply <in> <empty.opjl>` then rendered). No existing fixture touched.

- **How verified:** (all commands run on this aarch64 rig; `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`)
  - `-Werror` build, 0 warnings — after touching all four sources + tests:
    `cmake --build build -j4 | grep -icE "warning|error"` → `0`; `./build/op_tests` → OK.
  - Unit tests: `./build/op_tests` → `ALL PASS (0 failures)`; new block contributes 36 CHECKs (`cf.*`).
  - Defaults bit-exact — all pre-existing goldens unchanged: `bash harness/run_goldens.sh` →
    `run_goldens: 12 fixtures, 0 failures` (the 11 originals + the new `clip_fades`). Explicit `0.0/0.0` renders
    bit-identically to absent keys (`cf.explicit_zero_identical_to_absent`).
  - Render semantics — hard-left sampler over a constant-`0.25f` f32 asset at 120 bpm/48 kHz, every frame of the
    fade-in `[0,SPB)` equals `0.25f*float(double(f)/double(N))`, fade-out equals
    `0.25f*float(1.0-double(u)/double(M))` exactly (no tolerance), plateau/past-clip-end exactly `0.25f`, first
    frame exactly `0.0f` (`cf.A.*`, `cf.B.*`); same probe under tempo map `[{0,120},{2,60}]` with bounds from the
    equivalent of `TempoMap::at` widens N to 288000 and matches (`cf.F.*`). Per-clip scoping: unfaded sibling on the
    same track byte-equal (`cf.E.*`); `repeats 2` → one ramp at clip start + fade-out on last repeat (`cf.D.*`).
  - Op via engine — sets both / null clears one side / zeros restore never-faded canonical bytes
    (`cf.op.*`), every rejection returns the exact spec'd string with project unchanged (`rejected_unchanged`),
    History undo/redo byte-exact (`cf.hist.*`).
  - Op via CLI: `opd apply a.opj op.ok -o b.opj` exit 0; null clears one side (exit 0); out-of-range →
    `error: apply: line 1: set_clip_fades: clip.fade_in_beats must be a number in [0, 4096]`, **exit 5**, output not written.
  - Bad project value on render: `opd render bad.opj (fade_in_beats 9999) -o out.wav` →
    `error: parse: clip.fade_in_beats must be a number in [0, 4096]`, **exit 4**, nothing written.
  - `opd diff`: fades-only difference emits exactly one line
    `{"fade_in_beats":2.25,"fade_out_beats":3.5,"index":0,"op":"set_clip_fades"}`; applying it to a reproduces b's
    canonical bytes (`replay==canonical_b OK`); equal fades → 0 ops / no `set_clip_fades`; gain+fade difference emits
    `set_clip_gain` before `set_clip_fades` (`cf.diff.*`).
  - Harness: `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`;
    `bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - Sanitizers: `cmake -S . -B build-san -DDZ_SANITIZE=ON` + `cmake --build build-san --target op_tests -j4`
    (0 warnings) → `./build-san/op_tests` → `ALL PASS (0 failures)` (no ASAN/UBSAN reports).
  - ADR-0004: envelope uses only `+ - /` and casts — no clock, no randomness, no new libm call through `dsp.hpp`.

- **Metrics:** 4 interface files + tests + 2 golden files; 36 new CHECKs; build warnings 0; sanitizer op_tests clean.

- **PROPOSE / BLOCKED:** none.
