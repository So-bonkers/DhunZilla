---
id: WP-0136
title: Basic mixer — track mute/solo + master gain (model, render, stems, ops, goldens)
phase: 1
status: ready
depends_on: [WP-0135]
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
Phase 1 ("skeleton DAW") needs a basic mixer. Per-track gain and pan exist; mute, solo and a master
fader do not. This adds them to the project model, the deterministic renderer, stems export and the
operation layer (WP-0135), headless and golden-tested, so the future GUI mixer is a thin view.

## Spec — normative (append this section verbatim to `docs/spec/project-model.md`)
```markdown
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
```

Operations (amend `docs/spec/operations.md` table — add these three rows, keep everything else):
| `set_track_mute` | `index`, `mute` (bool) | |
| `set_track_solo` | `index`, `solo` (bool) | |
| `set_master_gain` | `gain_db` | `Project::master_gain_db` |

## Acceptance criteria
- [ ] `Track::mute`, `Track::solo`, `Project::master_gain_db` in `src/engine/model.hpp`; parse
      (type-checked: `mute`/`solo` must be JSON booleans) + canonical save omitting defaults.
- [ ] **Close a pre-existing validation gap** (found in WP-0135 review): the loader accepts any track
      `gain_db` and `pan` (e.g. +99 dB, pan 5.0, -1e9 all load and render today). Validate in the
      parser: track `gain_db` finite and in [-96, 24]; `pan` finite and in [-1, 1]; reject otherwise with
      `track.gain_db out of range [-96, 24]` / `track.pan out of range [-1, 1]`. Add these two rules to
      `docs/spec/project-model.md` (in the new mixer section). All existing goldens must still load
      (they use values inside the range). Ops inherit this automatically (they reuse the parser) — add one
      rejection test each for `set_track_gain 99` and `set_track_pan 5`.
- [ ] Render + stems per spec in `src/engine/render.cpp`.
- [ ] Three new ops in `src/engine/ops.cpp`, each with a success + rejection unit test; property
      test from WP-0135 includes them in its random op pool.
- [ ] **All existing goldens unchanged:** `bash harness/run_goldens.sh` green with zero fixture edits.
- [ ] New golden fixture `tests/fixtures/goldens/mixer.opj` + `mixer.wav` (this package **is
      authorized to add these two files**, not to modify any existing fixture): demo-like project with
      ≥3 tracks where one is muted, one is soloed, one is neither, and `master_gain_db: -6`.
      Register it in `harness/run_goldens.sh` the same way the others are. Render twice → bit-exact.
- [ ] Unit tests: (a) for each of mute-only, solo-only, mute+solo on the same track, and two solos:
      the render must be **bit-identical** to the render of a copy of the project in which the
      inaudible tracks were *deleted* (patterns re-pointed to the shifted track indices; master 0 dB); (b) master −6 dB: every sample equals the 0 dB
      render × `db_to_lin(-6)` exactly (same float op); (c) stems: inaudible stem all zeros, sum ≈ mix.
- [ ] `opd info <proj.opj>` prints `mute`/`solo` markers per track and `master_gain_db` when non-default.
- [ ] Docs: spec section appended to `docs/spec/project-model.md`; ops rows added to `docs/spec/operations.md`.
- [ ] All existing checks green (op_tests, goldens, cli_errors, glue scripts, sidecar pytest); zero new warnings.
- [ ] ROADMAP.md: WP-0136 row at the top of the work-package table.

## Out of scope — do NOT touch
- Sends, buses, routing matrix, inserts/effects, meters/LUFS (later packages).
- Existing golden fixtures (read-only), `ai-sidecar/`, `harness/job_client.py`, `spikes/`, `docs/adr/`.
- MIDI import/export behavior.

## Git
- Branch `wp-0136-mixer` from the latest `main`. Commit: `WP-0136: mixer mute/solo + master gain`.
  Include this task file with the handoff report filled in.
- **Do not push or merge.**

## Notes / landmines
- Bit-exactness at defaults is the whole game: skip work at default values rather than multiplying
  by 1.0f (see how clip gain and velocity do it — WP-0111/WP-0120 sections of project-model.md).
- `render_stems` must use the same audibility function as the mix — write it once (e.g.
  `bool track_audible(const Project&, size_t)` in model.hpp) and call it from both.

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` / `model.cpp`: `Track::mute`, `Track::solo` (bool, default false),
    `Project::master_gain_db` (default 0.0). Parser: type-checked (`track.mute must be a boolean` /
    `track.solo must be a boolean`), range-checked — master `[-96, 24]`
    (`master_gain_db out of range [-96, 24]`) **and the pre-existing gap closed**: track `gain_db`
    finite & `[-96, 24]` (`track.gain_db out of range [-96, 24]`), `pan` finite & `[-1, 1]`
    (`track.pan out of range [-1, 1]`). Canonical save omits all three fields at their defaults →
    legacy projects byte-identical. Single audibility function `bool track_audible(const Project&, size_t)`
    (mute wins: `!t.mute && (!any_solo || t.solo)`), called from both mix and stems paths.
  - `src/engine/render.cpp`: inaudible tracks skipped entirely in `render_subset` (stems get their
    all-zero full-length stem for free from the zero-init); master gain multiplies every output sample
    at the end of `render_subset` (covers mix + every stem), multiply skipped at exactly 0.0f.
    `render.hpp` doc comment updated (bit-identical stems sum now conditioned on master == 0).
  - `src/engine/ops.cpp`: three ops — `set_track_mute`, `set_track_solo`, `set_master_gain` — via the
    model-driven save→parse validation path; new `has_bool` arg check (`'mute' must be a boolean`).
  - `src/cli/main.cpp` (`opd info`): prints `master_gain_db=…` line when non-default and per-track
    `[mute]` / `[solo]` markers. No harness script pins `opd info` stdout (verified).
  - `tests/test_main.cpp`: `test_mixer()` — audibility rule table; mute-only / solo-one / mute+solo /
    two-solos renders **byte-identical** to project copies with inaudible tracks deleted (patterns
    re-pointed, master 0 dB), plus a negative control; master −6 dB == 0 dB render × `db_to_lin(−6)`
    bit-exact; stems: inaudible stem all zeros + sum ≈ mix with master on; parser rejections with exact
    error strings; canonical save omits defaults / round-trips / resave-stable. Ops success+rejection
    unit tests for the three new ops plus `set_track_gain 99` and `set_track_pan 5` gap-closure
    rejections. WP-0135 property-test random pool extended to pick all three (+3 cases).
  - Goldens (authorized): **added** `tests/fixtures/goldens/mixer.opj` + `mixer.wav` — 3 tracks
    (Click solo, Bassline mute, Pad neither), `master_gain_db: -6.0`, seed 136; auto-discovered by
    `run_goldens.sh`; doubles as the demo artifact for this audio-affecting WP. No existing fixture touched.
  - Docs: spec section appended verbatim to `docs/spec/project-model.md` (+ paragraph documenting the
    gain/pan/master validation rules); three rows added to `docs/spec/operations.md` table after
    `set_track_pan`; `ROADMAP.md` WP-0136 row at top of the work-package table.
- **How verified:** (all run locally on this aarch64 rig; full logs kept in `build/wp136_*.log`)
  - Fresh build, zero warnings: `cmake --build build -j` → `build rc=0 warnings/errors=0`.
  - Unit tests: `./build/op_tests` →
    ```
    PASS prop.workload_exercises_many_applies
    ALL PASS (0 failures)          # 306 PASS lines, rc=0
    INFO ops property: 500 sequences (len 1..40), 7299 applied, 3215 rejected
    ```
    New checks include `mixer.mute_only_equals_deletion`, `mixer.solo_one_equals_deletion`,
    `mixer.solo_two_equals_deletion`, `mixer.mute_plus_solo_is_silence`,
    `mixer.master_minus6_is_exact_scale`, `mixer.inaudible_stem_all_zeros`,
    `mixer.stems_sum_approx_mix_with_master`, `mixer.reject_track_gain_99`, `mixer.reject_track_pan_5`,
    `ops.set_master_gain_reject_range` (exact string `set_master_gain: master_gain_db out of range [-96, 24]`),
    `ops.set_track_gain_reject_range`, `ops.set_track_pan_reject_range`.
  - Golden renders (incl. new mixer fixture, twice-render bit-exact + golden match):
    ```
    PASS mixer: deterministic + golden match
    PASS negative-control (corrupt golden rejected)
    run_goldens: 7 fixtures, 0 failures          # bash harness/run_goldens.sh, rc=0
    ```
  - CLI error suite: `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  - Glue scripts: `bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - Sidecar pytest (untouched code, run for CI parity): `uv run pytest -q` in `ai-sidecar/` →
    `102 passed, 2 skipped, 57 warnings in 11.67s` (warnings pre-existing).
  - Note: bit-identity comparisons use memcmp of the sample bytes (like golden `cmp`), because engine
    voices deterministically emit NaN payloads and float `==` is false for NaN-vs-NaN — see follow-up.
- **Metrics:** 10 tracked files changed, +295/−7 lines; new golden fixture pair (mixer.opj 164 lines,
  mixer.wav 1.46 MB); op suite grew to 306 checks, all passing; property-test applied count 7299
  (up from ~7138 baseline) with the three new ops in the random pool.
- **Follow-ups noticed (not done):** Every golden WAV in the repo contains bit-deterministic NaN
  samples (demo.wav 1103, tempo.wav 7800 …), originating in pre-existing voice DSP paths (isolated to
  sine and synth voices with default params; unrelated to this WP — mute/skip path verified clean).
  Determinism is unaffected (goldens pass byte-exact) but third-party players may choke on NaN audio;
  worth a dedicated WP to find and clamp the inf/NaN source. Also: `opd info` output has no harness
  pin yet — fine for now, but a future UI-contract WP could lock it.
- **PROPOSE / BLOCKED:** None blocked. (No out-of-scope changes were made; the gain/pan validation
  gap was closed inside this WP as explicitly required by acceptance criterion 2.)
