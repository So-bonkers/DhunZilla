---
id: WP-0166
title: Pattern swing — optional per-pattern groove (render-time delay of odd 16ths) + set_pattern_swing op + diff
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.25
human_review: none
---

## Context — why this exists
Phase 1's exit criterion is "arrange a 3-minute beat with samples". Straight 16th hi-hats sound stiff; every
beat-maker has a swing/groove knob. This package adds an optional per-pattern `swing`: notes sitting exactly on
odd 16th steps are played slightly late **at render time only** (stored note times never change). Default 0
reproduces today's renders bit-exactly; no existing golden may change.

## Spec — normative
`docs/spec/project-model.md` (new section "Pattern swing — additive (WP-0166)") and `docs/spec/operations.md`
(new section "Pattern swing operation — additive (WP-0166)" plus an amendment to §`opd diff` step 7) are
**already updated by the runner** before you start. Read them first; they are the contract (units, which notes
swing, exact shift and rounding, validation, error strings, diff order). Do not edit those two spec files.
Summary (the spec sections win on any difference):

- Pattern JSON key `swing`: number in `[0, 0.75]`, a fraction, default 0.
- A note is swung iff its pattern-local `t` is exactly an odd 16th: `q = t * 4.0`, `std::floor(q) == q` and
  `std::fmod(q, 2.0) == 1.0`. Delay = `swing * 0.125` beats, added to the note's absolute beat position
  (`t_abs`, after the clip offset) **before** the existing `tmap.at(t_abs)` / `tmap.spb_at(t_abs)` calls in
  `render_pattern_at` in `src/engine/render.cpp`. No other rounding is introduced: the onset sample is whatever
  the renderer's existing rule gives for the shifted beat.
- Parse error (exact): `pattern.swing must be a number in [0, 0.75]` (non-number, negative, > 0.75).
- Canonical save omits `swing` when it is exactly 0.
- Op `set_pattern_swing {pattern, swing}`; `opd diff` emits it.

## Implementation guide (4 interface files + tests)
1. `src/engine/model.hpp` — `struct Pattern`: add `double swing = 0.0;  // render-time groove, [0, 0.75] (WP-0166)`.
2. `src/engine/model.cpp` — parse `swing` in the pattern loop right after the `pattern.track` check and before
   the notes (use `std::isfinite`; `#include <cmath>` if missing); emit `pj["swing"] = pat.swing` in
   `project_to_json` only when `pat.swing != 0.0`.
3. `src/engine/render.cpp` — in `render_pattern_at`, compute `t_abs` as today; then
   `if (pat.swing != 0.0 && is_odd_sixteenth(note.t_beats)) t_abs += pat.swing * 0.125;` (a small static helper
   in the anonymous namespace). Everything after that line is unchanged. With `swing == 0.0` the code path must
   be the existing one (no extra arithmetic on `t_abs`).
4. `src/engine/ops.cpp` — add the `set_pattern_swing` branch in `apply_named` (style of `set_sampler_trim`:
   `check_keys`, `arg_index`, `has_number`; write `j["patterns"][idx]["swing"]`; do NOT range-check, the model
   parse does). In `diff_projects`, after the per-pattern note ops of step 7, emit
   `{"op": "set_pattern_swing", "pattern": i, "swing": <b value or 0.0 if absent>}` when swing differs
   (absent = 0; use the existing `field_or` + `kZeroDb` helpers).
5. `tests/test_main.cpp` — new `static void test_pattern_swing()` called from `main`, **≥ 12 new CHECKs**,
   using a unique temp directory (`std::filesystem::temp_directory_path()` + a name containing the pid), never
   fixed `/tmp/op_*` paths: validation message for each bad kind (negative, 0.76, string, null); canonical save
   omits swing at 0 and keeps it at 0.5; swing 0 render equals no-swing render; with a sampler kick asset of
   known content, a 16th-note pattern at 120 bpm / 48 kHz and swing 0.5 puts the odd-step onsets at the
   expected sample (e.g. step 1: `llround((0.25 + 0.0625) * 24000)` = 7500) while even steps stay put; an
   off-grid note (`t = 0.3`) does not move; op sets / clears / rejects (`set_pattern_swing: pattern.swing must
   be a number in [0, 0.75]`); undo restores bytes; diff emits and applies the op.

## Acceptance criteria
- [x] Defaults bit-exact: every existing golden passes `bash harness/run_goldens.sh` unchanged (no new golden).
- [x] `opd render` of a hi-hat 16th pattern with swing in `{0.1, 0.5, 2/3, 0.75}` places each odd-step onset at
      exactly `TempoMap::at(t + swing*0.125)`, also under a tempo map; even steps and off-grid notes are unmoved.
- [x] `opd apply <proj> <journal>` with `{"op":"set_pattern_swing","pattern":0,"swing":0.5}` works; undo via the
      engine `History` restores the exact bytes; bad values are rejected with the spec'd strings.
- [x] `opd diff a.opj b.opj -o j.opjl` where the projects differ only in a pattern's swing emits that op, and
      applying it reproduces b.
- [x] Unit tests (≥ 12 new CHECKs, all pass); `build/op_tests` ends `ALL PASS`.
- [x] `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh` green; build has 0 warnings
      (`-Werror` default); sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` passes. Build with `-j4` max.
- [x] ADR-0004 respected: no clock/randomness, no libm; only the double arithmetic specified.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), existing golden fixtures (read-only; add none),
  `src/app/`, `ai-sidecar/`, `docs/adr/`, `docs/spec/*` (already updated).
- MIDI import/export, `opd analyze`, `step_grid`, and `opd steps` do not see swing (render-only by spec).
- Swing grids other than 16ths, per-note swing, swing curves, humanize/random timing.

## Git
- Branch `wp-0166-pattern-swing` from `main`. Commit: `WP-0166: pattern swing (render-time delay of odd 16ths) + set_pattern_swing op + diff`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` — `Pattern::swing` (`double swing = 0.0; // render-time groove, [0, 0.75] (WP-0166)`).
  - `src/engine/model.cpp` — parse `swing` in the pattern loop right after the `pattern.track` check and before
    the notes: non-number / non-finite / outside `[0, 0.75]` → exact string `pattern.swing must be a number in [0, 0.75]`
    (`<cmath>` was already included; `-0.0` accepted, equals 0). `project_to_json` emits `pj["swing"]` only when
    `pat.swing != 0.0` → canonical bytes of every existing project unchanged.
  - `src/engine/render.cpp` — anonymous-namespace helper `is_odd_sixteenth(t)` (`q = t*4.0; floor(q)==q && fmod(q,2.0)==1.0`,
    exact power-of-two scale). In `render_pattern_at`: `t_abs` computed as before, then
    `if (pat.swing != 0.0 && is_odd_sixteenth(note.t_beats)) t_abs += pat.swing * 0.125;` — one double addition,
    added last, before the untouched `tmap.at(t_abs)` / `tmap.spb_at(t_abs)` calls. `swing == 0.0` takes the exact
    old code path (no extra arithmetic).
  - `src/engine/ops.cpp` — `set_pattern_swing {pattern, swing}` branch in `apply_named` (strict `check_keys`,
    `arg_index`, `has_number`; range left to the model parse — no second ruleset). In `diff_projects` step 7, after
    each shared pattern's note ops: emit `{"op":"set_pattern_swing","pattern":i,"swing":<b value or 0.0>}` when
    swing differs (`field_or` + existing `kZeroDb`, absent = 0).
  - `tests/test_main.cpp` — new `test_pattern_swing()` (called from `main`), **18 new CHECKs**, unique temp dir
    `temp_directory_path()/op_swing_wp0166_<pid>` (added `#include <unistd.h>` for `getpid`). Note: the quoted-key
    search `"\"swing\""` is used in canonical-save checks because the fixture path `op_swing_kick.wav` itself contains "swing".
  - Spec files `docs/spec/project-model.md`, `docs/spec/operations.md` included unchanged as installed by the runner.
- **How verified:** (all commands run on this aarch64 rig; full outputs below)
  1. `-Werror` build, zero warnings: `cmake --build build -j4` → `[100%] Built target op_tests`, no warning lines.
  2. Unit tests: `./build/op_tests` → `ALL PASS (0 failures)` (705 CHECKs total; 18 new):
     ```
     PASS swing.parse_and_save_keeps_value            PASS ops.set_pattern_swing_sets
     PASS swing.e_negative                             PASS ops.set_pattern_swing_zero_clears_bytes
     PASS swing.e_above_range                          PASS ops.set_pattern_swing_reject_range
     PASS swing.e_string                               PASS ops.set_pattern_swing_reject_type
     PASS swing.e_null                                 PASS ops.set_pattern_swing_undo_restores_bytes
     PASS swing.minus_zero_treated_as_zero             PASS diff.swing_build_b_ok
     PASS swing.canonical_omits_zero                   PASS diff.swing_emits_and_replays_op
     PASS swing.render_zero_equals_no_swing_bitexact   PASS swing.onsets_odd_steps_shifted_even_unmoved
     PASS swing.off_grid_note_unmoved                  PASS swing.tempo_map_onsets_shifted_segment_by_position
     ```
  3. Goldens unchanged: `bash harness/run_goldens.sh` →
     `run_goldens: 9 fixtures, 0 failures` (all 9 `deterministic + golden match` + negative-control PASS).
  4. CLI contracts: `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  5. Glue e2e (`export UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`): `bash harness/test_glue_scripts.sh` →
     `test_glue_scripts: 0 failures`, exit 0.
  6. Sanitizers: existing `build-san` cache has `DZ_SANITIZE:BOOL=ON`; `cmake --build build-san -j4 --target op_tests`
     exit 0 (no warnings), `./build-san/op_tests` → `ALL PASS (0 failures)` (705 PASS under ASAN+UBSAN).
  7. CLI acceptance (scratch script `/home/shubhankar_dgx/qwen-scratch/wp0166-verify/verify.sh`, stereo-aware onset
     scan of PCM16 renders of a hard-left sampler 16th pattern, asset = constant 0.5 so every onset is a rising edge):
     ```
     PASS render_swing_0.1            # odd steps at llround((t+0.0125)*24000), even unmoved
     PASS render_swing_0.5            # step 1 → 7500 = llround((0.25+0.0625)*24000)
     PASS render_swing_0.6666666666666666
     PASS render_swing_0.75           # step 1 → 8250
     PASS off_grid_unmoved            # t=0.3 under swing 0.5 → onset 7200 (llround(0.3*24000))
     PASS tempo_map_onsets            # {bar0:120, bar1.95:60} → [7500, 187800, 196800]; shifted position picks segment
     PASS apply_op_writes_swing       # opd apply base.opj {"op":"set_pattern_swing","pattern":0,"swing":0.5}
     PASS diff_emits_op_exact         # edits.opjl == {"op":"set_pattern_swing","pattern":0,"swing":0.5}
     PASS diff_replay_reproduces_b    # apply(base, edits) → reverse diff is a 0-byte journal (exit 0)
     wp0166-verify: 0 failures
     ```
  8. ADR-0004: no clock/randomness touched; only the spec'd double arithmetic (`t*4.0`, `floor`, `fmod(q,2.0)`,
     one addition of `swing*0.125`) — same direct-rounding precedent as `on_grid` in ops.cpp and wav.cpp rounding
     (dsp.hpp wraps transcendental libm only). No new golden; no fixture touched.
  - Demo artifacts (audio-affecting feature, default path bit-identical): before/after WAVs at
    `/home/shubhankar_dgx/qwen-scratch/wp0166-verify/demo_before_no_swing.wav` and
    `demo_after_swing_0.5.wav` (same 16th hi-hat pattern, swing 0 vs 0.5).
- **Metrics:** +~40 lines engine across the 4 named interface files; 18 new CHECKs; 9/9 goldens bit-exact;
  705/705 unit checks pass native and under ASAN+UBSAN; all harness suites 0 failures.
- **PROPOSE / BLOCKED:** none blocked. No PROPOSE items — implementation followed the runner-installed spec sections exactly.
