---
id: WP-0150
title: Polyphase tap table for the sampler resampler — same output bits, much faster
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
`op::resample::windowsinc` (WP-0138, `src/engine/resample.cpp`) recomputes every kernel tap — one `sin` and two
`cos` via `dsp.hpp` — for every output sample. But `rem = (k × in_rate) % out_rate` is always a multiple of
`g = gcd(in_rate, out_rate)`, so `frac = rem / out_rate` takes only `out_rate / g` distinct values (160 for
44100 → 48000), and each tap depends only on the input offset `m = ip − i` and that phase. So the taps can be
computed once per call and reused. **The output must stay bit-identical** to today's for every rate pair
(goldens and determinism depend on it); the public API in `src/engine/resample.hpp` does not change.

## Spec — normative (append as a new subsection at the END of `docs/spec/project-model.md`)
```markdown
### Polyphase tap table — additive (WP-0150)
The converter evaluates taps from a per-call polyphase table instead of recomputing sin/cos per output
sample. Output is bit-identical to the direct method above for every rate pair; the public API is unchanged.
- rem = (k × in_rate) mod out_rate is a multiple of g = gcd(in_rate, out_rate), so there are only
  out_rate / g distinct fractions. Table entry (phase, m) holds the tap for input offset m = ip − i, built
  lazily with the same expressions and operation order as the direct method.
- Window bounds (lo/hi), clipping at the input edges, the normalising sum (summed in ascending i) and the
  accumulation order are unchanged.
- Memory bound: the table holds phases × (2·(ceil(half_w)+2)+1) doubles, at most 2^20 doubles (8 MiB).
  Above that the direct method is used instead (same bits).
```
(Do not alter any existing text in that file.)

## Implementation — `src/engine/resample.cpp` only
1. Keep the current body of `windowsinc` unchanged as a file-local function in an anonymous namespace:
   `bool windowsinc_direct(const std::vector<float>& in, int channels, int in_rate, int out_rate, std::vector<float>& out)`.
   It is the fallback and the normative reference.
2. New `windowsinc` (same signature) does the same argument checks, then:
   - `g = std::gcd(in_rate, out_rate)` (`<numeric>`), `phases = out_rate / g`, `R = ceil(half_w) + 2`, `W = 2R + 1`
     (`half_w` exactly as in the direct method).
   - If `phases > kMaxTableDoubles / W` (`constexpr long long kMaxTableDoubles = 1LL << 20;`) → `return windowsinc_direct(...)`.
   - Table: `std::vector<double>` of `phases × W`, plus a `filled` flag per phase. Row `ph = rem / g` is filled
     **lazily the first time that phase is used**, for `m = −R … +R`, entry index `m + R`:
     `d = static_cast<double>(m) + frac` (where `frac = static_cast<double>(rem) / static_cast<double>(out_rate)`),
     then the SAME `z`, `sincv`, `u`, `w`, `fc * sincv * w` expressions, in the SAME operation order, as the
     direct method. Do not reorder, factor out constants, or "simplify" any of that arithmetic.
   - Per output sample keep computing `ip`, `rem`, `frac`, `lo`, `hi` (with the same `ceil`/`floor`/clamping on the
     `double` values) exactly as the direct method. Tap for input `i` is `row[(ip − i) + R]`. If the window would
     fall outside the table (`ip − lo > R` or `ip − hi < −R`), `return windowsinc_direct(...)` for the whole call.
   - `hsum`: sum the used taps with `+=` in ascending `i` starting from `0.0` (same order as before). Optional
     speed-up: cache it per phase when the window is unclipped and `lo − ip` / `hi − ip` match the cached ones.
   - Channel accumulation unchanged: `acc += double(in[i*channels+c]) * tap`, ascending `i`, `static_cast<float>(acc / hsum)`.
   - `if (!(hsum != 0.0)) continue;` and `if (lo > hi) continue;` behave as before (output stays 0).
3. Determinism: no new libm use outside `dsp.hpp`; no new compiler flags; no threads.

## Tests (`tests/`)
- New header `tests/resample_reference.hpp`: a TEST-ONLY frozen copy of today's direct algorithm as
  `inline bool op_test_ref::windowsinc_direct_ref(const std::vector<float>&, int, int, int, std::vector<float>&)`
  (copy it from the current `resample.cpp` BEFORE you change that file; add `namespace dsp = op::dsp;` inside
  your namespace so the `dsp::` calls resolve). Header comment: do not optimize this file.
- In `tests/test_main.cpp` add `static void test_resample_polyphase()` (call it right after `test_resample()` in
  `main`; `#include "resample_reference.hpp"`). Compare `op::resample::windowsinc` vs the reference
  **bit-for-bit** (`std::memcmp` on the float buffers + equal return value) with seeded pseudo-random input
  (fixed-seed LCG, no `rand`) over: in_rate ∈ {8000, 11025, 22050, 32000, 44100, 88200, 96000, 192000} → 48000,
  plus 44101 → 48000 (exercises the over-budget fallback) and 48000 → 44100; channels 1 and 2; input lengths
  {0, 1, 2, 17, 100, 4801}. Every CHECK name must start with `resample.polyphase_` (e.g.
  `resample.polyphase_bit_identical_matrix`). Also a CHECK that the output vector is fully overwritten when passed
  pre-filled garbage.

## Acceptance criteria
- [x] `windowsinc` output is bit-identical to the pre-change algorithm for all rates/lengths/channel counts.
- [x] `src/engine/resample.hpp` unchanged; no other source file changed.
- [x] Unit tests above pass; ≥ 2 new CHECKs named `resample.polyphase_*`.
- [x] 10 s stereo 44100 → 48000 is at least 8× faster than the direct method (measure it with a throwaway
      program, report the number in the handoff; do not commit timing code or assertions).
- [x] Spec subsection appended.
- [x] Standard gates: build with 0 warnings (`-j4` max), `op_tests`, `bash harness/run_goldens.sh`,
      `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`, and a sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests`, all green. Existing goldens must be unchanged.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row), `src/engine/resample.hpp`, other engine files, `src/cli/`, `src/doc/`,
  `ai-sidecar/`, `docs/adr/`, anything under `tests/fixtures/`.
- Changing the filter design, window, cutoff, rounding, or output length; SIMD; threads; float tables.

## Git
- Branch `wp-0150-resample-polyphase` from `main`. Commit: `WP-0150: polyphase tap table for the sampler resampler`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/resample.cpp`: old `windowsinc` body moved verbatim into file-local
    `windowsinc_direct` (anonymous namespace; fallback + normative reference). New `windowsinc`
    builds a per-call lazy polyphase tap table: `g = std::gcd(in_rate, out_rate)`,
    `phases = out_rate / g`, `R = ceil(half_w)+2`, `W = 2R+1`; over-budget
    (`phases > kMaxTableDoubles / W`, `kMaxTableDoubles = 1LL << 20`) or an off-table window
    (defensive) → whole call falls back to `windowsinc_direct`. Row `rem / g` filled on first use
    for `m = −R…+R` with the same expressions and operation order as the direct method; per-sample
    `ip/rem/frac/lo/hi`, `hsum` (ascending `i` from 0.0), accumulation order, edge clipping and the
    two defensive `continue`s unchanged. `<numeric>` added; no new libm use outside `dsp.hpp`.
  - `tests/resample_reference.hpp` (new): TEST-ONLY frozen copy of the pre-change direct algorithm
    (`op_test_ref::windowsinc_direct_ref`, header says do-not-optimize).
  - `tests/test_main.cpp`: `#include "resample_reference.hpp"`, new `test_resample_polyphase()`
    called right after `test_resample()`. Fixed-seed LCG (no `rand`) input; memcmp bit-compare vs
    reference + equal return value over in_rate ∈ {8000, 11025, 22050, 32000, 44100, 88200, 96000,
    192000} → 48000, plus 44101 → 48000 (over-budget fallback) and 48000 → 44100; channels 1/2;
    lengths {0, 1, 2, 17, 100, 4801}; plus pre-filled-garbage overwrite CHECK. 11 new CHECKs, all
    named `resample.polyphase_*`.
  - `docs/spec/project-model.md`: appended the "Polyphase tap table — additive (WP-0150)" subsection
    verbatim at the END; no existing text altered.
- **How verified:** (include the measured speedup and the number of bit-compared cases)
  - Bit-identity: 121 converter calls compared bit-for-bit (10 rate pairs × 2 channels × 6 lengths
    = 120, + 1 pre-filled-garbage case), **272 007 float scalars** memcmp-compared vs the frozen
    reference — `./build/op_tests` (and identical results under ASAN+UBSAN):
    ```
    PASS resample.polyphase_bit_identical_8000_to_48000
    PASS resample.polyphase_bit_identical_11025_to_48000
    PASS resample.polyphase_bit_identical_22050_to_48000
    PASS resample.polyphase_bit_identical_32000_to_48000
    PASS resample.polyphase_bit_identical_44100_to_48000
    PASS resample.polyphase_bit_identical_88200_to_48000
    PASS resample.polyphase_bit_identical_96000_to_48000
    PASS resample.polyphase_bit_identical_192000_to_48000
    PASS resample.polyphase_bit_identical_44101_to_48000_over_budget
    PASS resample.polyphase_bit_identical_48000_to_44100
    PASS resample.polyphase_prefilled_garbage_overwritten
    INFO resample polyphase: 272007 float scalars bit-compared vs reference
    ALL PASS (0 failures)        # exit 0, full suite incl. all pre-existing resample tests
    ```
  - **Measured speedup — 13.4×** (requirement ≥ 8×). Throwaway program in /tmp (not committed),
    both implementations compiled into one binary at `-O2 -ffp-contract=off`, 10 s stereo
    44100 → 48000 (480 000 out frames), 3 runs; `bit_identical=1` asserted in the same run:
    ```
    direct=0.248 s polyphase=0.019 s speedup=13.4x bit_identical=1 out_frames=480000
    direct=0.254 s polyphase=0.019 s speedup=13.6x bit_identical=1 out_frames=480000
    direct=0.248 s polyphase=0.018 s speedup=13.4x bit_identical=1 out_frames=480000
    ```
  - Gates, all green on this branch: `cmake --build build -j4` (0 warnings, `-Werror` on);
    `./build/op_tests` ALL PASS (exit 0);
    `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures` — existing goldens
    unchanged, incl. `PASS resampled: deterministic + golden match`;
    `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`;
    `UV_CACHE_DIR=… bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`;
    `-DDZ_SANITIZE=ON` rebuild + `./build-san/op_tests` → `ALL PASS (0 failures)`.
  - Scope check: `git diff --stat` touches only `src/engine/resample.cpp`, `tests/test_main.cpp`,
    `docs/spec/project-model.md` (+ new `tests/resample_reference.hpp`); `git diff --exit-code
    src/engine/resample.hpp` → clean ("resample.hpp UNCHANGED").
- **PROPOSE / BLOCKED:** none. (Note for the ROADMAP elephant: WP-0150 row.)
