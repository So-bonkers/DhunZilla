---
id: WP-0145
title: opd render --from-bar / --to-bar — bit-exact bar-range render (tempo-map aware)
phase: 1
status: done
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context — why this exists
Producers iterate on one section of a song (a drop, bars 16–24). Today `opd render` always renders the
whole project. This package adds a range render whose output is *exactly* a slice of the full render,
so a range is never different audio from the same bars in the full song. It needs one shared engine
function for bar → sample (including the tempo map) so CLI, tests and later GUI agree.

## Spec — normative (append as a new section to `docs/spec/project-model.md`)
The full normative section is provided in the repo as `docs/spec/project-model.md` **already containing
"## Render a bar range — additive (WP-0145)"** (the runner installs it). Read it; it is the contract. Summary:
- Bars are **0-based** (like `start_bar`); fractional allowed; defaults A=0, B=`length_bars`.
- `S(bar)` = `total_samples_for(p)` if `bar >= length_bars`, else `min(at(bar*beats_per_bar), total)`, with
  `at` = the renderer's existing tempo-map beat→sample function (`TempoMap::at` in `src/engine/render.cpp`).
- Output = exactly frames `[S(A), S(B))` of the full render; data chunk byte-equal to the full render's
  slice; also for the mix and every stem when `--stems-dir` is given.
- Validation errors: `error: usage: <detail>`, exit 2, exact detail strings in the spec section.

## Implementation (exact names)
1. `src/engine/render.hpp` / `render.cpp`: add
   ```cpp
   // Bar -> sample (spec "Render a bar range"). 0-based bar, fractional ok. Uses the renderer's own tempo map.
   long long sample_at_bar(const Project& p, double bar);
   // Frames [from_frame, to_frame) of `buf` copied into `out` (same sample_rate). Requires 0 <= from < to <= frames.
   void slice_frames(const wav::StereoBuffer& buf, long long from_frame, long long to_frame, wav::StereoBuffer& out);
   ```
   `sample_at_bar` must reuse `build_tempo_map(...).at(...)` (do not re-implement the math).
2. `src/cli/main.cpp` `cmd_render`: parse `--from-bar A` and `--to-bar B` (use `std::strtod`; the whole
   argument must be consumed and the value finite). Validate per the spec, then render fully (existing
   code), slice with `slice_frames` using `sample_at_bar`, and write. Do the same slicing for every stem
   and the mix in `--stems-dir` mode. Without the flags, behaviour and output stay byte-identical to today.
   Add the two options to the `usage()` text for `opd render`. The success line stays
   `rendered <frames> frames @<rate> Hz -> <path> (seed=<n>)` where `<frames>` is the sliced frame count.
3. `tests/test_main.cpp`: ≥ 6 new CHECKs, e.g. `sample_at_bar` for a constant-tempo project (bar 1 of
   120 bpm 4/4 @48k = 96000; bar `length_bars` = `total_samples_for`), for `tests/fixtures/goldens/tempo.opj`
   (bar 4 = 384000; bar 5 = 384000 + llround(4·48000·60/72) = 544000), and `slice_frames` contents.
4. `harness/test_cli_errors.sh`: add cases (exit 2) for `--from-bar abc`, `--from-bar 2 --to-bar 1`,
   `--to-bar 99` on demo.opj, and one success case (exit 0) `--from-bar 0.5 --to-bar 1.5`.
5. Spec: already installed; do not edit it except to fix a typo you are certain about.

## Acceptance criteria
- [x] `opd render demo.opj -o r.wav --from-bar 0.5 --to-bar 1.5` → data chunk byte-equal to the matching
      slice of the full render; frame count `S(1.5)-S(0.5)`.
- [x] Same for `tempo.opj` ranges crossing the tempo change (e.g. 3 → 6, and 4 → 8) and `arranged.opj`.
- [x] `--stems-dir` + range: mix and stems are slices of the full ones.
- [x] No flags → output unchanged (`bash harness/run_goldens.sh` green).
- [x] Error cases per spec (exit 2, `error: usage: ...`), no output file created on error.
- [x] Determinism: same range render twice → `cmp`-identical.
- [x] Standard gates: build with 0 warnings (`-j4` max), `op_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests`.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds rows at ship time), existing fixtures in `tests/fixtures/goldens/`,
  `ai-sidecar/`, `docs/adr/`, the render algorithm itself (only add the two functions), other CLI commands.
- Efficient partial rendering, looping, cross-fades, sample-rate conversion, GUI.

## Git
- Branch `wp-0145-render-range` from `main`. Commit: `WP-0145: opd render --from-bar/--to-bar bit-exact bar range`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/render.hpp` / `render.cpp`: added the two spec-named functions. `sample_at_bar(p, bar)` reuses the
    renderer's own `build_tempo_map(p, p.sample_rate).at(bar * beats_per_bar)` (no re-implemented math), clamps
    `bar >= length_bars` to `total_samples_for(p)`, and `min`s with total otherwise. `slice_frames(buf, from, to, out)`
    copies frames `[from, to)` preserving `sample_rate`. Includes `<algorithm>`/`<cstddef>` added.
  - `src/cli/main.cpp`: `usage()` render line now lists `[--from-bar A] [--to-bar B]`; arg loop parses both options
    (option without a value falls through to the existing `unknown arg '<arg>'`). New `parse_bar_value()` helper:
    `std::strtod`, whole argument consumed, finite. Validation order per spec — syntax (`bad --…-bar value '<arg>'`,
    `--from-bar must be >= 0`, `--from-bar must be less than --to-bar`) → project load (exit 3/4 unchanged) → bounds
    (`--to-bar <B as given> exceeds length_bars`, `--from-bar <A as given> is not before the end of the song`,
    `bar range is empty`). Render stays full-length; `slice_frames` then cuts mix + every stem in `--stems-dir` mode.
    Without flags no slicing happens at all — code path byte-identical to before. Success lines report sliced frame
    counts via the existing printfs.
  - `tests/test_main.cpp`: new `test_render_range()` — 10 CHECKs (`range.*`): bar 1 @120bpm = 96000, fractional bars
    0.5/1.5 → 48000/144000, bar==length_bars == `total_samples_for`, past-end clamp, `tempo.opj` fixture bar 4 = 384000 /
    bar 5 = 544000 (crosses the 120→72 tempo change), `slice_frames` shape/contents/full-range identity. Called from `main()`.
  - `harness/test_cli_errors.sh`: +5 checks — exit 2 for `--from-bar abc`, `--from-bar 2 --to-bar 1`, `--to-bar 99`
    (each with a no-output-file check) and exit 0 success `--from-bar 0.5 --to-bar 1.5` on demo.opj.
  - `docs/spec/project-model.md`: runner-installed normative section committed unchanged. No fixtures touched.
- **How verified:** (all commands run on this aarch64 DGX Spark; scratch outputs under `/home/shubhankar_dgx/qwen-scratch/tmp-wp0145/`)
  - Gates: `cmake --build build -j4` → `[100%] Built target op_tests`, **0 warning lines** (`-Werror` ON).
    `./build/op_tests` → `ALL PASS (0 failures)` incl. all 10 new `range.*` checks.
    `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures` (no-flag output byte-identical — goldens gate).
    `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures` (incl. the new range cases).
    `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
    Sanitizers: `cmake --build build-san -j4 && ./build-san/op_tests` (ASAN+UBSAN) → `ALL PASS (0 failures)`.
  - Byte-equality vs full render (python; canonical 44-byte header — all bytes equal except the RIFF/data size fields,
    which verified encode the range file's sizes little-endian; data chunk compared):
    ```
    demo 0.5->1.5:     data bytes equal [48000,144000)   (384000B vs 384000B)
    tempo 3->6:        data bytes equal [288000,704000)  (1664000B vs 1664000B)
    tempo 4->8 (end):  data bytes equal [384000,1024000) (2560000B vs 2560000B)
    arranged 1->3:     data bytes equal [120000,360000)  (960000B vs 960000B)
    mix (stems mode):  data bytes equal [48000,144000)   (384000B vs 384000B)
    stem 0_metronome / 1_bassline / 2_kick: all PASS at the same slice — compare.py exit 0
    ```
    Frame counts printed by opd match `S(B)-S(A)` exactly: demo range 96000 (full 192000); tempo 3→6 → 416000,
    4→8 → 640000 (full 1024000); arranged 240000 (full 480000).
  - Error contract (stderr captured verbatim; every case exit 2 and no file created):
    ```
    --from-bar abc            error: usage: bad --from-bar value 'abc'
    --to-bar 1x               error: usage: bad --to-bar value '1x'
    --to-bar nan / inf        error: usage: bad --to-bar value 'nan' ('inf')
    --from-bar (no value)     error: usage: unknown arg '--from-bar'
    --from-bar -1             error: usage: --from-bar must be >= 0
    --from-bar 2 --to-bar 1   error: usage: --from-bar must be less than --to-bar
    --to-bar 99 (demo)        error: usage: --to-bar 99 exceeds length_bars
    --from-bar 2 (demo)       error: usage: --from-bar 2 is not before the end of the song
    --to-bar 0                error: usage: bar range is empty
    ```
    Missing project with `--to-bar 99` still exits 3 (`error: not_found:`) — load-order preserved.
  - Determinism: `opd render tempo.opj -o r.wav --from-bar 3 --to-bar 6` twice → `cmp` identical.
- **Metrics:** +2 engine functions (~25 lines), ~40 CLI lines, 10 unit CHECKs, 5 harness checks; no fixture/golden edits.
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE (out of scope here): when efficient partial rendering lands it must
  keep `S(bar)` semantics and can drop the full-render+slice cost; a GUI section-loop should call `op::sample_at_bar`.
