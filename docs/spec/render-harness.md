# Spec: Golden-File Harness

The harness is the project's immune system: it turns "sounds right?" into exit codes.

## Tools
`harness/golden_diff.py <a.wav> <b.wav> [--mode bitexact|dbfs] [--tol-dB X]`
- `bitexact`: byte-for-byte after stripping nothing (canonical header included). Exit 0/1.
- `dbfs`: reports per-channel RMS dBFS delta, peak, max abs sample delta; fails if max-abs > tol (default 1e-7).
- Prints one machine-parsable line: `RESULT pass|fail mode=... max_abs_delta=... rms_db_a=... rms_db_b=...`

`harness/run_goldens.sh` — for every fixture pair in `tests/fixtures/goldens/` (`*.opj` + expected `*.wav`):
render twice with `opd render`, assert bit-exact run-to-run, then diff vs golden. Any failure → exit 1 with which fixture failed.

## Rules
- Golden WAVs are committed under `tests/fixtures/goldens/`. Regenerating a golden requires the work package to say so explicitly, and the PR must show the before/after dBFS diff (AGENTS.md).
- The harness itself is pure-python stdlib + numpy (no torch) — it must run on any CI runner in seconds.
- Determinism contract under test: same binary, two renders → identical; golden pair → identical across x86_64 and aarch64 runners (float determinism guard).

## Render output sample formats — additive (WP-0149)
`opd render <project.opj> -o <out.wav> [--seed N] [--stems-dir DIR] [--format pcm16|pcm24|f32]`.
`--format` selects the sample format of EVERY WAV the command writes (the mix, and each stem when
`--stems-dir` is given). Default `pcm16`: output is byte-identical to the pre-WP-0149 writer (all goldens
unchanged). The rendered float buffer is identical for all formats; only the final conversion differs.
Format names are lowercase and case-sensitive. Any other value (including empty) → nothing is written,
stderr `error: usage: unknown format '<value>' (expected pcm16, pcm24 or f32)`, exit 2. Only the space-separated form `--format f32` exists; `--format` as the last argument with no value is the existing
`unknown arg` usage error (exit 2).

All formats are stereo, little-endian, at the project sample rate (`SR`), `F` = frames.

**pcm16** — unchanged: 44-byte header, fmt tag 1, bits 16, block align 4, byte rate `SR*4`, data `F*4` bytes,
RIFF size `36 + F*4`. Conversion: `float_to_pcm16` (saturating round-half-to-even of `x*32768`, NaN -> 0).

**pcm24** — 44-byte header, same layout as pcm16 except: bits = 24, block align = 6, byte rate = `SR*6`,
data size = `F*6`, RIFF size = `36 + F*6`. Each sample is a 3-byte two's-complement little-endian integer.
Conversion `int32_t float_to_pcm24(float x)` mirrors pcm16 exactly: NaN -> 0; `x <= -1.0` -> -8388608;
`x >= 1.0` -> 8388607; otherwise `scaled = x * 8388608.0f` (exact), round half to even to an integer
(`floor`; fraction > 0.5 up, < 0.5 down, exactly 0.5 to the even neighbour), then clamp to
[-8388608, 8388607]. Equivalently `clamp(round_half_even(x * 8388608), -8388608, 8388607)` for non-NaN x.
Bytes of a value v: `v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF` (arithmetic shift on the signed value).

**f32** — IEEE float, plain (NOT WAVE_FORMAT_EXTENSIBLE). 58-byte header, then data:
| offset | size | value |
|---|---|---|
| 0 | 4 | `RIFF` |
| 4 | 4 | RIFF size = `50 + F*8` |
| 8 | 4 | `WAVE` |
| 12 | 4 | `fmt ` |
| 16 | 4 | 18 (fmt chunk size) |
| 20 | 2 | 3 (WAVE_FORMAT_IEEE_FLOAT) |
| 22 | 2 | 2 channels |
| 24 | 4 | `SR` |
| 28 | 4 | byte rate `SR*8` |
| 32 | 2 | block align 8 |
| 34 | 2 | bits 32 |
| 36 | 2 | cbSize = 0 |
| 38 | 4 | `fact` |
| 42 | 4 | 4 (fact chunk size) |
| 46 | 4 | dwSampleLength = `F` (frames per channel) |
| 50 | 4 | `data` |
| 54 | 4 | data size `F*8` |
| 58 | ... | `F*2` float32 values, interleaved L,R |
Samples are the engine's float values written as-is: no scaling, no clipping (values beyond +/-1.0 are
preserved), bit pattern copied verbatim (including NaN payloads and -0.0).

**Reader (`wav::read_wav`)** additionally accepts 24-bit PCM (fmt tag 1, bits 24; sample = `int24 * (1/8388608.0f)`,
exact). 16-bit PCM and 32-bit float behave as before; `fact` and other unknown chunks are skipped. So
`opd info` and the sampler accept pcm24 and f32 files written by `opd render`. Other bit depths remain
rejected with `PCM must be 16- or 24-bit`.

Cross-format invariants (tested): for every sample x of an `f32` render, the `pcm24` render holds
`float_to_pcm24(x)` and the `pcm16` render holds `float_to_pcm16(x)`, exactly.
