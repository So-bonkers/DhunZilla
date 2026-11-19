---
id: WP-0138
title: Sampler accepts any WAV sample rate — deterministic windowed-sinc resampling at load
phase: 1
status: done
depends_on: []
estimate_agent_hours: 1.5
human_review: none
---

## Context — why this exists
Phase 1's exit criterion is "arrange a 3-minute beat with samples". Today the sampler voice plays assets
nearest-neighbour with **no resampling — 48 kHz assets only** (`docs/spec/project-model.md`, sampler
voice). Most real sample packs are 44.1 kHz, so they can't be used. This package resamples sampler assets
once, at load time, to the project rate — deterministically (bit-exact across x86_64/aarch64).

## Spec — normative (append as a new section to `docs/spec/project-model.md`)
```markdown
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
```

## Acceptance criteria
- [x] Implementation in the engine (resample once per asset path into the existing sampler cache; no
      per-render-block work). Mono and stereo assets.
- [x] **All existing goldens unchanged** (`bash harness/run_goldens.sh` green; their kits are 48 kHz).
- [x] Unit tests in `tests/test_main.cpp`:
      (a) 48 kHz asset → buffer bit-identical to the pre-change loader output;
      (b) 1 kHz sine at −6 dBFS generated at 44100 Hz (2 s) → resampled to 48000: estimate frequency by
          zero-crossing count over the middle 1.5 s → within ±0.2 Hz of 1000; SNR vs an ideal 48 kHz sine
          (best-fit phase) ≥ 70 dB, ignoring the first and last 64 output samples;
      (c) 96000 → 48000 with a 30 kHz tone (above output Nyquist) → output RMS ≤ −60 dB relative to input
          (aliasing rejected); (d) output length rule exact for 44100→48000 and 96000→48000;
      (e) unsupported rate 4000 Hz → render exit 5 with the spec'd message.
- [x] Determinism: render a project using a 44.1 kHz asset twice → `cmp`-identical WAVs.
- [x] New golden `tests/fixtures/goldens/resampled.opj` + `resampled.wav` + kit asset
      `tests/fixtures/goldens/kits/kick44k.wav` (44.1 kHz, ≤ 0.5 s; generate it deterministically with a
      small script you commit under `harness/` or reuse an existing kit sample converted offline by a
      script). This package **is authorized to add these three files**, not modify existing fixtures.
      Register in `harness/run_goldens.sh` like the others.
- [x] Spec section appended; ROADMAP.md row at the top of the table.
- [x] All checks green (op_tests, goldens, cli_errors, glue scripts); 0 warnings. Build with `-j4` max.

## Out of scope — do NOT touch
- Pitch-shifting/time-stretching, playback-rate changes, real-time resampling, other voices.
- Existing fixtures (read-only), `ai-sidecar/`, `docs/adr/`, `src/cli/` (unless only usage text).

## Git
- Branch `wp-0138-sampler-resample` from the current HEAD of this worktree (already the correct base — do not check out, pull or merge `main`). Commit: `WP-0138: sampler resamples non-native-rate assets (windowed sinc, deterministic)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/resample.hpp` / `.cpp` (new): fixed windowed-sinc converter per the spec section — Blackman
    window over ±16 zero crossings of the input sinc (stretched ×1/min(1,out/in) when downsampling),
    fc = 0.97·min(1,out/in) of input Nyquist; input position per output sample from `(k·in_rate)/out_rate`
    (64-bit integer part + double fraction, no drift); length = exact-integer `ceil(in_frames·out/in)`;
    taps outside the input zeroed and each output normalized by the used-tap sum (DC gain 1 at edges);
    double accumulation via new `dsp::kPi_d/sind_/cosd_` wrappers, float32 storage. Mono + stereo interleaved.
  - `src/engine/render.cpp`: in `render_sampler`, a cache miss on a non-native-rate asset now checks the
    8000–192000 Hz policy (`sampler: unsupported sample rate <n>` → exit 5) and resamples **once** into the
    existing sampler-cache entry; playback path untouched, so zero per-render-block work. Native-rate assets
    take the original branch verbatim (bit-identical).
  - `src/engine/dsp.hpp`: added double `kPi_d/sind_/cosd_` wrappers (still the single libm chokepoint, ADR-0004).
  - `CMakeLists.txt`: `resample.cpp` added to `opengine`.
  - `tests/test_main.cpp`: `test_resample()` — rate bounds inclusive; (a) native 48 kHz mono+stereo render
    bit-identical to loader output; (b) −6 dBFS 1 kHz @44100→48000 zero-crossing freq estimate + best-fit-phase SNR;
    stereo channels bitwise-equal to mono result; (c) 30 kHz tone 96000→48000 alias rejection; (d) length rule
    on dividing and non-dividing counts; (e) 4000 Hz → render failure with exact spec message.
  - `harness/make_resampled_golden.py` (new, committed): deterministic stdlib synth kick @44100 Hz, 15434 frames
    (0.35 s ≤ 0.5 s) → `kits/kick44k.wav`. New goldens `resampled.opj`/`resampled.wav`: 4 notes of the 44.1 kHz
    kit in a 48 kHz project; picked up by `run_goldens.sh`'s existing `*.opj` glob (same registration as all others).
  - `docs/spec/project-model.md`: spec section appended verbatim from this package. `ROADMAP.md`: WP-0138 row
    added at the top of the work-package table. No existing fixtures, no `ai-sidecar/`, no `src/cli/` touched.
- **How verified:** (include the measured frequency error, SNR, alias rejection numbers)
  - Clean rebuild, `-j4`, 0 warnings: `cmake --build build --target clean && cmake --build build -j4` → grep of
    full output for `warning|error` matched nothing; `Built target opengine/opd/op_tests`.
  - `./build/op_tests` → `ALL PASS (0 failures)`. Resampler section (measured values as required):
    ```
    PASS resample.rate_bounds_inclusive            PASS resample.rate_out_of_range_rejected
    PASS resample.native_48k_bit_identical_to_loader   PASS resample.native_48k_stereo_bit_identical_to_loader
    PASS resample.out_len_441_to_48_exact          INFO resample freq_est=1000.000000 Hz err=0.000000 Hz
    PASS resample.b_freq_within_0p2_hz             INFO resample snr_vs_ideal_sine=107.00 dB
    PASS resample.b_snr_ge_70db                    PASS resample.stereo_channels_match_mono_bitwise
    PASS resample.out_len_96k_to_48k_exact         INFO resample alias_30k_rms=-90.75 dB rel input
    PASS resample.c_alias_rejected_le_minus60db    PASS resample.out_len_ceil_rule_1000f_441_to_48
    PASS resample.out_len_ceil_rule_96001f_to_48001   PASS resample.e_error_message_exact
    ```
    → frequency error **0.000000 Hz** (≤ 0.2), SNR vs ideal sine **107.00 dB** (≥ 70), alias rejection
    **−90.75 dB** (≤ −60). Length rule: 88200 f→96000, 1000 f@44.1k→1089, 96001 f@96k→48001 — all exact.
  - `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures` — the 7 pre-existing goldens still
    bit-exact (kits are 48 kHz → untouched path) and new `PASS resampled: deterministic + golden match`.
    Negative control PASS. Demo artifact = the authorized golden pair (`resampled.opj`→`resampled.wav`).
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`. End-to-end exit-5 contract (4000 Hz kit):
    ```
    $ ./build/opd render /tmp/dz_lowrate/low.opj -o /tmp/dz_lowrate/out.wav
    error: render: sampler: unsupported sample rate 4000   → CLI_EXIT: 5
    ```
  - Determinism (44.1 kHz asset): two renders of `resampled.opj` → `cmp` identical, and byte-identical to the
    committed golden (`MATCHES COMMITTED GOLDEN`). Kit generator reproducible: re-run of
    `harness/make_resampled_golden.py` reproduced `kits/kick44k.wav` at sha256
    `8e562a046cce41b14f0477fcb76f4920e31042cedc46ddb9852541837d0e7e1a` (same as committed).
  - `bash harness/test_glue_scripts.sh` (with `UV_CACHE_DIR=<workspace>/.cache/uv`) →
    `test_glue_scripts: 0 failures`, exit 0. Note: the first run on a cold uv cache failed 3 checks with
    "sidecar never announced a port" (venv sync exceeded the startup timeout); warm re-run green twice —
    environmental, unaffected by this package (no sidecar files touched).
  - ASAN+UBSAN (`cmake -B build-san -DDZ_SANITIZE=ON`, mirrors the CI sanitizers job): clean build, `op_tests`
    `ALL PASS (0 failures)`, `run_goldens.sh build-san/opd` → 8 fixtures 0 failures,
    `test_cli_errors.sh build-san/opd` → 0 failures.
- **PROPOSE / BLOCKED:** nothing blocked. PROPOSE: CI glue-scripts job could pre-warm the sidecar venv/uv cache
  (or raise the port-announce timeout) — cold-cache runs flake as above on fresh runners. Resampler cost is
  ~32 taps × per-sample double math at load; fine for kits, a future big-library path may want float SIMD or
  cached polyphase tables (out of scope here).

