---
id: WP-0149
title: Render output sample formats — opd render --format pcm16|pcm24|f32
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context — why this exists
`opd render` only writes 16-bit PCM. Mastering/export and stem hand-off want 24-bit PCM and 32-bit float
output. The engine renders float anyway; this package adds the final conversion choices. Default stays
`pcm16`, byte-identical to today (all goldens unchanged).

## Spec — normative
The full normative text is the new section "Render output sample formats — additive (WP-0149)" in
`docs/spec/render-harness.md` (already replaced by the runner with the final version; do not reword it,
re-read it first). Summary of the exact names you must use:

### `src/engine/wav.hpp` (add; keep every existing declaration and call site working)
```cpp
enum class SampleFormat { Pcm16, Pcm24, F32 };

// Saturating round-half-to-even float -> signed 24-bit value in [-8388608, 8388607].
// Mirrors float_to_pcm16 exactly with scale 8388608: NaN -> 0; x <= -1 -> -8388608; x >= 1 -> 8388607;
// else round_half_even(x * 8388608.0f) clamped.
int32_t float_to_pcm24(float x);

// Writes `buf` as stereo WAV in the given format (header layouts in the spec section).
bool write_wav_stereo(const std::string& path, StereoBuffer& buf, SampleFormat fmt, std::string& err);
```
`write_pcm16_stereo(path, buf, err)` stays and becomes a one-line wrapper calling
`write_wav_stereo(path, buf, SampleFormat::Pcm16, err)`; its output must stay byte-identical.
Header fields (exact): pcm24 = 44-byte header, tag 1, bits 24, block align 6, byte rate SR*6, data F*6,
RIFF size 36+F*6, samples as 3-byte little-endian two's complement. f32 = 58-byte header: plain `fmt ` chunk of
size 18 (tag 3, 2 ch, SR, byte rate SR*8, block align 8, bits 32, cbSize 0), then a `fact` chunk (size 4,
value F), then `data` (size F*8); RIFF size 50+F*8; NOT extensible; floats copied bit-for-bit via `memcpy`
(no clipping, no NaN rewriting). Use explicit little-endian byte assembly like the existing writer; write in
chunks like the existing writer (no giant allocation).

`read_wav`: also accept fmt tag 1 with bits 24 (`int24 * (1.0f/8388608.0f)`, sign-extend correctly).
Float32 with a `fact` chunk must already work (unknown chunks are skipped) — keep it so. Other PCM depths
still rejected, with the message changed to `PCM must be 16- or 24-bit`. Keep the strict fmt validation
(no out-of-bounds reads).

### CLI (`src/cli/main.cpp`, `opd render` only)
`opd render <project.opj> -o <out.wav> [--seed N] [--stems-dir DIR] [--format pcm16|pcm24|f32]`.
Default `pcm16`. The chosen format applies to the mix AND every stem file. Invalid value → no file written,
stderr `error: usage: unknown format '<value>' (expected pcm16, pcm24 or f32)` (use the existing `fail("usage", ...)`
helper: it prints `error: usage: <detail>` and returns exit 2; print the usage text as other usage errors do),
exit **2**. Validate during argument parsing, before the project is loaded. Update the `usage()` line.
Stdout messages are unchanged.

## Acceptance criteria
- [x] Engine + CLI as specified; `write_pcm16_stereo` call sites untouched.
- [x] **All existing goldens unchanged** (`bash harness/run_goldens.sh` green; no fixture edits).
- [x] ≥ 8 new CHECKs in `tests/test_main.cpp`: `float_to_pcm24` table (0, ±1, 2, NaN, half-even ties such as
      `0.5/8388608`, `1.5/8388608`, `2.5/8388608`, and a negative tie); pcm24 and f32 files written by
      `write_wav_stereo` have the exact byte header (size, tag, bits, block align, byte rate, RIFF size, `fact`
      value for f32) and round-trip through `read_wav` (pcm24 sample == `float_to_pcm24(x)/8388608.0f`,
      f32 bit-exact including values 2.5 and -3.0 with no clipping); `write_pcm16_stereo` output equals
      `write_wav_stereo(..., Pcm16, ...)` output byte-for-byte.
- [x] `harness/test_cli_errors.sh`: add `check bad_format 2 "$OPD" render <demo.opj> -o "$TMP/o.wav" --format mp3`
      (use any fixture path style already used in that file).
- [x] `docs/spec/render-harness.md` is the runner-provided version (do not edit further, except typo-free
      consistency).
- [x] Determinism: each format rendered twice is `cmp`-identical.
- [x] Gates: build with 0 warnings (`-Werror`), `op_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, and a sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` all green. `-j4` max.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds rows at ship time), existing fixtures in `tests/fixtures/`, `ai-sidecar/`,
  `docs/adr/`, `harness/golden_diff.py`, dithering, sample-rate conversion on output, mono output, other
  `opd` subcommands, any `--format` meaning on commands other than `render`.

## Git
- Branch `wp-0149-render-formats` from `main`. Commit: `WP-0149: render output sample formats (pcm16|pcm24|f32)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/wav.hpp` / `wav.cpp`: added `enum class SampleFormat { Pcm16, Pcm24, F32 }`,
    `int32_t float_to_pcm24(float)` (saturating round-half-to-even at scale 8388608, NaN→0 — mirrors
    `float_to_pcm16`), and `bool write_wav_stereo(path, buf, fmt, err)` with per-format headers exactly per the
    spec section (pcm24: tag 1 / bits 24 / align 6 / RIFF 36+F*6; f32: 58-byte header, plain fmt size 18 tag 3
    cbSize 0, `fact` size 4 value F, data F*8, RIFF 50+F*8 — not extensible). Data written in 4096-frame chunks;
    f32 payload is the raw bit pattern (explicit LE byte assembly from a `memcpy`'d uint32 — no clipping, NaN
    preserved). `write_pcm16_stereo` is now the one-line Pcm16 wrapper. `read_wav` accepts fmt tag 1 bits 24
    (manual 3-byte LE assembly + sign extension × `1.0f/8388608.0f`; deliberately not `rd_u32_le`, which would
    read 1 byte past the chunk) and rejects other PCM depths with the new message
    `PCM must be 16- or 24-bit`. Strict fmt validation from WP-0125 kept intact; f32-with-`fact` reads via the
    existing unknown-chunk skip.
  - `src/cli/main.cpp`: `opd render [--format pcm16|pcm24|f32]`, default pcm16, validated during arg parsing
    **before** project load (invalid → nothing written); format applies to mix and every stem (the three
    `cmd_render` write calls now use `write_wav_stereo(..., fmt, ...)`). `usage()` render line updated. All
    other `write_pcm16_stereo` call sites (gen-testproject kick.wav) untouched; stdout messages unchanged.
  - `tests/test_main.cpp`: new `test_wav_formats()` registered in `main()` — **15 new CHECKs** (8-entry
    `float_to_pcm24` table incl. ties `0.5/8388608→0`, `1.5/8388608→2`, `2.5/8388608→2`, negatives `-0.5→0` /
    `-1.5→-2`; exact byte headers for pcm24/f32 incl. `fact` value and RIFF sizes; round-trips through
    `read_wav` — pcm24 sample == `float_to_pcm24(x)/8388608.0f`, f32 bit-exact incl. 2.5 and −3.0; wrapper
    byte-identity).
  - `harness/test_cli_errors.sh`: added `check bad_format 2 "$OPD" render "$ROOT/tests/fixtures/goldens/demo.opj"
    -o "$TMP/o.wav" --format mp3` + a no-output-created check.
  - `docs/spec/render-harness.md`: runner-provided version committed unchanged (not reworded).
- **How verified:** (all on aarch64, g++ 13, `-j4`; real output below)
  1. `-Werror` build clean (`DZ_WERROR:BOOL=ON`, zero warnings): `cmake --build build -j4` → `[100%] Built target op_tests`.
  2. Unit tests: `./build/op_tests` → `ALL PASS (0 failures)` — 426 CHECKs, incl. the 15 new ones
     (`pcm24.zero/floor/saturation/nan_maps_zero/half_even_down_to_0/half_even_up_to_2/half_even_down_to_2/negative_ties`,
     `fmt.pcm24_write_ok/pcm24_header_bytes_exact/pcm24_roundtrip_exact/f32_write_ok/f32_header_bytes_exact/f32_roundtrip_bitexact_no_clipping/pcm16_wrapper_byte_identical`).
  3. Goldens: `bash harness/run_goldens.sh` → `run_goldens: 8 fixtures, 0 failures` + `PASS negative-control (corrupt golden rejected)`.
  4. CLI contract: `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures` (incl. new `bad_format`).
     Verbatim stderr of `./build/opd render tests/fixtures/goldens/demo.opj -o /tmp/should_not_exist.wav --format mp3`:
     `error: usage: unknown format 'mp3' (expected pcm16, pcm24 or f32)` — exit 2, output file not created.
  5. Glue e2e: `bash harness/test_glue_scripts.sh` (with `UV_CACHE_DIR` in workspace) → `test_glue_scripts: 0 failures`.
  6. Sanitizers: `cmake --build build-san -j4` (`-DDZ_SANITIZE=ON`) clean, `./build-san/op_tests` → `ALL PASS (0 failures)` under ASAN+UBSAN.
  7. Determinism (demo.opj, 192000 frames @48 kHz, two renders each, `cmp`):
     ```
     determinism pcm16: identical run-to-run (size 768044 bytes)   # 44 + F*4
     determinism pcm24: identical run-to-run (size 1152044 bytes)  # 44 + F*6
     determinism f32:   identical run-to-run (size 1536058 bytes)  # 58 + F*8
     default == explicit --format pcm16: identical
     pcm16 == golden demo.wav: byte-identical (goldens unchanged)
     ```
     Stems inherit the format (`--stems-dir … --format f32` → all three stems `fmt-tag=0x03 bits=32`; mix in
     stems-mode `cmp`-identical to a plain f32 render).
  - Note found & fixed during verification: first f32 iteration wrote `n*4` instead of `n*8` bytes per chunk
    (truncated data section); caught by the exact-header CHECK, fixed, full suite re-run green.
- **Metrics:** diff stat — 6 files, +256/−26 (`wav.cpp` +112/−26 mostly writer restructure; `test_main.cpp` +82;
  `main.cpp` +16/−4; `wav.hpp` +14; `test_cli_errors.sh` +4; spec file is the runner's, +54).
- **PROPOSE / BLOCKED:** none. (Sidecar pytest/ruff untouched — no `ai-sidecar/` changes; CI covers them.)
