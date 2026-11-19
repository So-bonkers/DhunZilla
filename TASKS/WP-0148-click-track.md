---
id: WP-0148
title: `opd click` — render a deterministic metronome click track for the project's full length
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context — why this exists
Producers want a click WAV that follows the project's tempo map (to record against, or to align stems).
Today the only click is the `metronome` voice, which needs hand-placed notes. This package adds a
standalone command that renders clicks for every beat of the project, using the renderer's own
beat→sample mapping so clicks land exactly on the sample positions notes would.

## Spec — normative (append as a new section to `docs/spec/project-model.md`; the exact text is provided)
Append this section verbatim at the end of `docs/spec/project-model.md` (the file is also supplied in
full as a replacement by the runner; make sure it ends up identical in your branch):
```markdown
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
```

## Implementation notes (exact names — do not invent others)
- `src/engine/render.hpp` / `render.cpp`: declare and define in `namespace op`
  `std::vector<long long> beat_onsets(const Project& p);` and
  `bool render_click(const Project& p, wav::StereoBuffer& out, std::string& err);`.
- **Reuse the existing `TempoMap`/`build_tempo_map`/`tm.at(double beats)` in `render.cpp`** (anonymous
  namespace) for the onset: `at(static_cast<double>(b))`. Do NOT reimplement the rounding. The note-onset
  path is `tmap.at(t_abs)` in `render_subset`. `beat_onsets` loops `b = 0, 1, ...` and stops at the first
  `at(b) >= total_samples_for(p)`.
- `render_click` validates length exactly like `render_subset` (`total <= 0` →
  `render_error: zero-length render`; > 600 s → `render_error: project exceeds 600s offline cap`), sets
  `out.sample_rate = p.sample_rate`, sizes `out.interleaved` to `total*2` zeros, then writes bursts.
  The burst is written (assigned, not added) to both channels. Compute per sample, with `L = sr*30/1000`
  (`long long`), `tau = sr / 200.0` (double): `ph = 2.0 * dsp::kPi_d() * f * n / sr` (all double),
  `env = dsp::expf_(-(float)(n / tau))`, `x = 0.5f * (float)dsp::cosd_(ph) * env`. Use only `dsp.hpp`
  math (no `std::cos`/`std::exp` directly). The beat index `i` in `beat_onsets` order is the beat number
  `b`; downbeat iff `i % p.beats_per_bar == 0`.
- `src/cli/main.cpp`: new `cmd_click` + dispatch `click`, patterned on `cmd_render` (same arg parsing:
  `-o` required, unknown arg → usage exit 2; not found → 3; parse → 4; render → 5; WAV write via
  `wav::write_pcm16_stereo`, failure → exit 5). Add a usage line `  opd click <project.opj> -o <out.wav>`.
  Print nothing on stdout on success.
- Do not change `render_project`/`render_stems` behaviour.

## Acceptance criteria
- [x] Spec section appended verbatim to `docs/spec/project-model.md`.
- [x] `opd click` works as specified: for `tests/fixtures/goldens/demo.opj` and `tempo.opj` the output has
      exactly `total_samples_for` frames, onsets/bursts per spec.
- [x] Unit tests in `tests/test_main.cpp` (≥ 4 new CHECKs): onsets for demo-like constant tempo are
      `0, 24000, 48000, ...`; for a tempo-map project (120 bpm to bar 4 then 72 bpm) beat 16 is at
      sample 384000 and beat 17 at 424000; `render_click` frame count equals `total_samples_for`;
      first burst sample is `0.5f` on both channels at an onset and downbeat burst differs from beat
      burst at `n = 1`; two calls produce identical buffers.
- [x] CLI: add to `harness/test_cli_errors.sh`: `opd click` with no `-o` → 2, missing project → 3,
      unparseable project → 4, valid project → 0 and output file exists.
- [x] Existing goldens untouched and green; no new golden required.
- [x] Standard gates: build with 0 warnings (`-Werror`), `op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`, and an
      ASAN/UBSAN build of `op_tests` (`-DDZ_SANITIZE=ON`) all green. Build with `-j4` max.

## Out of scope — do NOT touch
- ROADMAP.md (the elephant adds the row), `tests/fixtures/` (read-only), `ai-sidecar/`, `docs/adr/`,
  other docs/spec files, the `metronome` voice and its output, mixer/mute/solo semantics, MIDI code.
- Click customization (sounds, accents patterns, subdivisions, count-in) — deferred.

## Git
- Branch `wp-0148-click-track` from `main`. Commit: `WP-0148: opd click renders a tempo-map-aware metronome track`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/render.hpp` / `render.cpp`: new `op::beat_onsets(const Project&)` — absolute sample
    position of every integer beat b while `at(b) < total_samples_for(p)`, reusing the existing
    `TempoMap`/`build_tempo_map` (no rounding reimplemented) — and `op::render_click(const Project&,
    wav::StereoBuffer&, std::string&)`: full-length stereo click track, validation identical to
    `render_subset` (`"render_error: zero-length render"`, `"render_error: project exceeds 600s offline cap"`),
    bursts per spec §Click track render (L = sr*30/1000 int div, tau = sr/200.0, double phase via
    `dsp::kPi_d()`/`dsp::cosd_`, envelope via `dsp::expf_`, 1500 Hz downbeat / 1000 Hz beat, peak
    0.5 assigned to both channels). `render_project`/`render_stems` untouched.
  - `src/cli/main.cpp`: `cmd_click` patterned on `cmd_render` (usage line added; `-o` required → 2,
    missing project → 3, parse → 4, render/write → 5), dispatch for `click`, nothing printed on stdout
    on success.
  - `tests/test_main.cpp`: new `test_click()` with 9 CHECKs (was ≥4 required).
  - `harness/test_cli_errors.sh`: 5 new cases (`click_no_out`, `click_missing_proj`, `click_parse_error`,
    `click_ok`, `click_output_written`).
  - `docs/spec/project-model.md`: spec section only (appended verbatim as provided; verified byte-identical:
    `verbatim match: True` via a task-file-vs-spec suffix comparison).
- **How verified:** (all commands from repo root, `-j4`)
  - `cmake --build build -j4` — clean, zero warnings with `DZ_WERROR=ON` (default).
  - `./build/op_tests` → `ALL PASS (0 failures)`; new CHECKs:
    `PASS click.onsets_constant_tempo_0_24000_48000`, `PASS click.onsets_tempo_map_beat16_17`,
    `PASS click.render_ok`, `PASS click.length_matches_total_samples`,
    `PASS click.onset_sample_is_half_both_channels`, `PASS click.downbeat_differs_from_beat_at_n1`,
    `PASS click.samples_outside_bursts_exactly_zero`, `PASS click.rerender_ok`,
    `PASS click.determinism_bit_identical_rerun`.
  - `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures` (fixtures untouched — `git status` shows no changes under `tests/fixtures/`).
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`, incl.
    `PASS click_no_out (exit 2)`, `PASS click_missing_proj (exit 3)`, `PASS click_parse_error (exit 4)`,
    `PASS click_ok (exit 0)`, `PASS click_output_written`.
  - `bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures` (with `UV_CACHE_DIR=<workspace>/.cache/uv`).
  - ASAN+UBSAN: `cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4 && ./build-san/op_tests` → `ALL PASS (0 failures)`.
  - Fixture-level spec check (python3 + wave, both fixtures): exit 0 with empty stdout; rerun byte-identical;
    frames == `total_samples_for` (`demo.opj: 192000`, `tempo.opj: 1024000`); L/R bit-identical; every sample
    outside bursts exactly 0; onset sample == 0.5 quantized (16384) both channels as first non-zero of each burst;
    burst samples match the spec formula to **0 LSB** max deviation over all bursts (demo: 8 bursts onsets
    `[0, 24000, 48000, 72000, ...]`; tempo: 32 bursts, beat16=384000, beat17=424000, BurstL=1440) →
    `click_fixture_check: 0 failures`.
- **PROPOSE / BLOCKED:** none.
