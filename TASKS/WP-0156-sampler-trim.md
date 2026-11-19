---
id: WP-0156
title: Sampler trim — play a slice of an asset (optional start_s / end_s per sampler voice) + set_sampler_trim op
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.25
human_review: none
---

## Context — why this exists
Phase 1's exit criterion is "arrange a 3-minute beat with samples". A sampler voice today always plays an
asset from its first frame to its last. Real sample packs ship long files (a loop, a drum break, a multi-hit
file) of which a producer wants one part. "Slicing-lite": two optional per-voice numbers choose the part of
the WAV that plays. Defaults reproduce today's behaviour **bit-exactly**; no existing golden may change.

## Spec — normative
The two spec files `docs/spec/project-model.md` (new section "Sampler trim — additive (WP-0156)") and
`docs/spec/operations.md` (new section "Sampler trim operation — additive (WP-0156)") are **already
updated by the runner** before you start. Read both new sections first; they are the contract (units,
exact rounding rule, validation order, error strings, clamp-vs-error decisions, op argument rules).
Do not edit those two spec files. Summary (the spec sections win on any difference):

- Voice JSON keys `start_s` (number, default 0.0) and `end_s` (number, default absent = end of asset),
  sampler voices only. Seconds are converted with `llround(seconds × sample_rate)` to frame indices of the
  project-rate buffer **after resampling** (WP-0138 runs first, unchanged). Slice = `[i0, i1)`,
  `i1` clamped to the buffer length.
- Parse errors (exact): `voice.start_s must be a finite number >= 0`, `voice.end_s must be a finite number > 0`,
  `voice.end_s must be greater than voice.start_s`, `voice.start_s/end_s are only valid for sampler voices`.
- Render error (exact, exit 5): `sampler: trim slice is empty` when `i1 <= i0` after clamping.
- Playback = today's semantics on the slice: note plays `min(i1 − i0, llround(dur × spb))` frames starting at
  slice frame 0; no loop, no fade.
- Canonical save emits `start_s` only when `!= 0.0` and `end_s` only when present.
- Op `set_sampler_trim {index, start_s, end_s}` (`end_s` may be `null` = no end), strings per the spec section.

## Implementation guide (files, 5 interface files)
1. `src/engine/model.hpp` — add to `struct Voice`: `double start_s = 0.0; double end_s = 0.0; bool has_end_s = false;`
   (exactly these names).
2. `src/engine/model.cpp` — parse in `parse_voice` (validation order as in the spec; use `std::isfinite`, add
   `#include <cmath>`); emit in `voice_to_json` for the sampler kind only when non-default. Keep key
   insertion simple; `project_to_json` canonical output must stay byte-identical for untrimmed voices.
3. `src/engine/render.cpp` — `render_sampler` takes the `Voice&` (add a parameter after `note`) and computes
   `i0`/`i1` AFTER the cache entry is loaded/resampled. When `start_s == 0.0 && !has_end_s` skip all trim
   arithmetic (`i0 = 0`, `i1 = avail`) so the default path is the existing one. Sample reads use `i0 + n`.
4. `src/engine/ops.cpp` — add the `set_sampler_trim` branch in `apply_named` next to the other track ops
   (same style as `set_track_pan`; use `check_keys`, `arg_index`, `has_number`). Do NOT re-implement range
   checks: the model parse enforces them. Do not touch `diff_projects`.
5. `tests/test_main.cpp` — new `static void test_sampler_trim()` called from `main`, **≥ 10 new CHECKs**:
   exact slice samples (a 1000-frame asset whose frame `i` holds `i/32768.0f`, PCM16-exact), silence after the
   slice, note shorter than the slice (gating), `end_s` beyond the asset clamps, start beyond asset →
   render error string, each parse error string, canonical save omits default keys, op sets / clears
   (`end_s: null`) / rejects, a 44.1 kHz asset trimmed (just assert it renders and the slice length is
   `i1 − i0` frames at 48 kHz), and a default-trim render equal to the previous render path.

## New golden (authorized: add exactly these four files, nothing else under `tests/fixtures/`)
1. `harness/make_trim_golden.py` — pure-stdlib, integer-only generator for the asset below (model it on
   `harness/make_resampled_golden.py`; header docstring says how to regenerate).
2. `tests/fixtures/goldens/kits/ramp48k.wav` — mono PCM16 **48000 Hz**, 4800 frames, frame `i` = int16 value
   `5 * i` (i = 0..4799). Canonical 44-byte header (RIFF/WAVE/fmt/data, no other chunks).
3. `tests/fixtures/goldens/sampler_trim.opj` — canonical JSON (alphabetically sorted keys, 2-space indent, as
   the other goldens): `beats_per_bar 4, bpm 120.0, format_version 0, length_bars 1.0, sample_rate 48000, seed 7`;
   one track `{"gain_db": 0.0, "name": "RampSlice", "pan": 0.0, "voice": {"end_s": 0.0375, "kind": "sampler",
   "path": "kits/ramp48k.wav", "start_s": 0.0125}}`; one pattern for track 0 with two notes:
   `{"dur": 1.0, "t": 0.0}` and `{"dur": 0.01, "t": 1.0}`. (Slice = frames 600..1799, 1200 frames; the second
   note gates it at 240 frames.)
4. `tests/fixtures/goldens/sampler_trim.wav` — produced by `build/opd render` of (3). `harness/run_goldens.sh`
   discovers goldens by glob; do not edit it. Never modify any existing fixture.

## Acceptance criteria
- [ ] Defaults bit-exact: all 8 pre-existing goldens + `sampler_trim` pass `bash harness/run_goldens.sh`.
- [ ] `build/opd render tests/fixtures/goldens/sampler_trim.opj` left channel at output frame `n < 1200`
      is the ramp value of source frame `600 + n` (× equal-power pan 0.7071…), silence from frame 1200 to the
      second note at 24000, then 240 frames of source frames `600..839`, then silence.
- [ ] `opd apply` with `{"op":"set_sampler_trim","index":0,"start_s":0.01,"end_s":0.02}` works; rejections
      leave the file unwritten and exit 5 with the spec'd strings.
- [ ] Unit tests (≥ 10 new CHECKs, all pass); `build/op_tests` prints `ALL PASS`.
- [ ] `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh` green; build has 0 warnings
      (`-Werror` default); sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` passes. Build with `-j4` max.
- [ ] ADR-0004 respected: no clock/randomness; `llround` and `double` arithmetic only as specified.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), `docs/spec/*` (already updated by the runner),
  existing fixtures, `harness/run_goldens.sh`, `src/cli/` (no CLI change needed), `src/doc/`, `src/app/`,
  `ai-sidecar/`, `docs/adr/`, the resampler itself, `diff_projects`.
- Looping, pitch/rate change, fades, per-note trim, slicing by transients, UI.

## Git
- Branch `wp-0156-sampler-trim` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0156: sampler trim (start_s/end_s slice) + set_sampler_trim op`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp`: `Voice` gains `start_s`, `end_s`, `has_end_s`.
  - `src/engine/model.cpp`: `parse_voice` parses/validates `start_s`/`end_s` (exact spec strings, order: sampler-only check, start, end, end-vs-start); `voice_to_json` emits `start_s` only when != 0.0 and `end_s` only when present, sampler kind only. Added `#include <cmath>`.
  - `src/engine/render.cpp`: `render_sampler` takes `const Voice& v` (after `note`). Trim indices are computed after the cache entry is loaded/resampled; the default (`start_s == 0 && !has_end_s`) skips all trim arithmetic. Slice `[i0,i1)`, `i1` clamped to `avail`, `i1 <= i0` -> `sampler: trim slice is empty`. Sample reads use `i0 + n`. (Implementation detail beyond the spec text: the double is clamped to 4e18 before `llround` so absurd values stay defined; no effect on any in-range value.)
  - `src/engine/ops.cpp`: `set_sampler_trim` branch next to `set_track_pan`: `check_keys`, `arg_index`, `has_number`; `end_s` must be number or null (null erases the key); non-sampler -> `track is not a sampler voice`. Range checks left to the model parse.
  - `tests/test_main.cpp`: `test_sampler_trim()` (33 CHECKs whose names contain `trim`, including `ops.set_sampler_trim_*`), called from `main`.
  - New golden: `harness/make_trim_golden.py`, `tests/fixtures/goldens/kits/ramp48k.wav` (sha256 205087fc2a32094b6a990e07991e7ad1c91d7ec051dbdb450ff72b778eebc517, 9644 bytes = 44 + 4800*2), `tests/fixtures/goldens/sampler_trim.opj`, `tests/fixtures/goldens/sampler_trim.wav` (rendered by `build/opd render`).
  - `docs/spec/*.md` left as installed by the runner (unchanged by me). ROADMAP, src/cli, src/doc, src/app, ai-sidecar, docs/adr, resampler, diff_projects untouched.
- **How verified:**
  - Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2` -> no warnings (-Werror applied to our targets), `Built target op_tests`, `Built target opd`.
  - Goldens: `bash harness/run_goldens.sh` (with `UV_CACHE_DIR` exported):
    ```
    PASS arranged / demo / imported / mixer / panorama / pitched / resampled / tempo: deterministic + golden match
    PASS sampler_trim: deterministic + golden match
    PASS negative-control (corrupt golden rejected)
    run_goldens: 9 fixtures, 0 failures
    ```
    The 8 pre-existing goldens pass unchanged (defaults bit-exact).
  - Golden slice values (left channel, PCM16, pan 0.7071 applied): output n=0 -> 2121 (= 5*600*0.7071), n=1199 -> 6360 (= 5*1799*0.7071), n=1200 -> 0; frame 24000 (second note) -> 2121; n=239 at 24239 -> 2966 (= 5*839*0.7071); 24240 -> 0; nonzero count in 1200..23999 = 0; nothing nonzero after 24240.
  - Unit tests: `./build/op_tests` -> `ALL PASS (0 failures)` (553 PASS lines total).
  - Sanitizers: `cmake -S . -B build-asan -DDZ_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug && cmake --build build-asan -j2 --target op_tests && ./build-asan/op_tests` -> `ALL PASS (0 failures)`.
  - `bash harness/test_cli_errors.sh` -> `test_cli_errors: 0 failures` (exit 0). `bash harness/test_glue_scripts.sh` -> `test_glue_scripts: 0 failures` (exit 0).
  - CLI: `opd apply t.opj ok.opjl -o out.opj` with `set_sampler_trim 0.01/0.02` -> exit 0, voice `"end_s": 0.02, "start_s": 0.01`. Rejected `start_s 0.03, end_s 0.02` -> `error: apply: line 1: set_sampler_trim: voice.end_s must be greater than voice.start_s`, exit 5, output file not created. `set_sampler_trim` with `start_s 0, end_s null` on a trimmed file -> output byte-identical to the untrimmed canonical file.
- **PROPOSE / BLOCKED:** none. Interpretation note: for a non-sampler voice carrying `start_s`/`end_s`, the sampler-only error is reported before any value check (the spec lists it as a precondition; the value checks are "only when either key is present on a sampler voice").
