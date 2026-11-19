---
id: WP-0158
title: AIFF / AIFF-C reader — sampler assets and `opd info` accept AIFF as well as WAV
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
---

## Context — why this exists
Sample packs ship as AIFF as often as WAV, and the prototype goal ("arrange a 3-minute beat with samples")
needs them usable. Today `op::wav::read_wav` (`src/engine/wav.cpp`) only parses RIFF/WAVE. This package
adds an AIFF reader that returns the very same `ReadResult`, so the sampler (including its WP-0138
resampling) and `opd info` accept AIFF with no other change. The WAV reader is strict on purpose
(WP-0125 fuzz hardening); the AIFF reader must be equally strict. **WAV behaviour must not change.**

Decision on AIFF-C: supported, because it is a few lines — compression type `NONE` (big-endian PCM, same as
plain AIFF) and `sowt` (the same PCM but little-endian). Every other AIFF-C compression is rejected.

## Spec — normative
### API (`src/engine/wav.hpp`, namespace `op::wav`)
1. Add a field to `ReadResult`, **after** `error`: `std::string format = "wav";` (`"wav"` or `"aiff"`).
2. `ReadResult read_aiff(const std::string& path);` — sets `format = "aiff"` (also on failure).
3. `ReadResult read_audio(const std::string& path);` — dispatch **by file signature, never by extension**:
   open the file; if it has at least 4 bytes and they are `FORM` → `return read_aiff(path)`; otherwise
   (including unopenable, short or empty files) → `return read_wav(path)` unchanged. So every WAV result
   and error string is byte-for-byte what `read_wav` produced before. `read_wav` itself is not modified.
   Doc comments with a usage example, like the existing declarations.

### `read_aiff` (in `src/engine/wav.cpp`)
Read the whole file. Big-endian helpers `rd_u16_be`/`rd_u32_be`, explicit byte assembly (no struct
casts, no `memcpy` into multi-byte ints). Ignore the FORM size field (like the WAV reader ignores the RIFF
size). Walk chunks from offset 12: 4-byte id, 4-byte big-endian size, body, one pad byte if the size is
odd. **If a chunk's declared size runs past the end of the file, stop scanning** (same as the WAV reader).
Unknown chunks (`NAME`, `ANNO`, `MARK`, `INST`, `COMT`, `FVER`, …) are skipped. The **first** `COMM`
and the **first** `SSND` win; later duplicates are ignored.

`COMM` body: channels u16, numSampleFrames u32, sampleSize u16, sample rate = 80-bit IEEE extended
(10 bytes); for `AIFC` also compressionType (4 bytes) + pascal-string name (ignored).
`SSND` body: offset u32, blockSize u32 (ignored), then data; the sample data starts `offset` bytes after
the 8-byte header of the body.

Validation, **in exactly this order**, first failure wins. Each error is returned in `ReadResult::error`
(so `opd info` prints `error: parse: <text>` and exits 4; the sampler reports `sampler: <text>`, exit 5):

| # | condition | error text (exact) |
|---|-----------|--------------------|
| 1 | file cannot be opened | `cannot open '<path>'` |
| 2 | file size < 12 | `file too small for AIFF` |
| 3 | bytes 0–3 ≠ `FORM`, or bytes 8–11 not `AIFF`/`AIFC` | `not a FORM/AIFF file` |
| 4 | (during scan) `COMM` body size < 18 (`AIFF`) or < 22 (`AIFC`) | `COMM chunk too short` |
| 5 | (during scan) `SSND` body size < 8 | `SSND chunk too short` |
| 6 | (during scan) `SSND` offset > body size − 8 | `truncated sound data` |
| 7 | after the scan, no COMM or no SSND found | `missing COMM/SSND chunk` |
| 8 | channels not 1 or 2 | `only mono/stereo supported` |
| 9 | `AIFC` and compression not `NONE`/`sowt` | `unsupported AIFF-C compression` |
| 10 | sampleSize not 16 or 24 (so 8-bit is rejected) | `AIFF must be 16 or 24-bit` |
| 11 | sample rate invalid (below) | `implausible sample rate` |
| 12 | `numSampleFrames × channels × (sampleSize/8)` > bytes available after the offset | `truncated sound data` |

(Compute 12 in 64-bit.) A file with 0 frames is valid (empty `samples`). Rows 4–6 trigger while scanning,
before row 7; the scan stops at the first such error. Note a file cut off in the middle of its `SSND` data
therefore ends in row 7 (chunk runs past EOF → scan stops → SSND not found).

**Sample rate (80-bit extended), decoded exactly with integer arithmetic only — no `ldexp`, no double:**
sign+exponent u16 `se`, mantissa u64 `m` (explicit integer bit, big-endian). Invalid (→ row 11) if the sign
bit is set, exponent field is `0x7FFF` (inf/NaN), or `m == 0`. Otherwise value = `m · 2^e` with
`e = exp − 16383 − 63`. If `e ≥ 0`: invalid when `e ≥ 20` or `m > (1000000 >> e)`, else `v = m << e`.
If `e < 0`, `s = −e`: invalid when `s ≥ 64` (value < 1) or any of the low `s` bits of `m` are set
(non-integer rate); else `v = m >> s`. Finally invalid if `v < 1` or `v > 1000000` (the WAV reader's
bound). Examples: 44100 = `400E AC44 0000 0000 0000`; 48000 = `400E BB80 0000 0000 0000`;
8000 = `400B FA00 0000 0000 0000`. Non-normalised encodings of an integer rate decode fine.

**Samples** → `ReadResult{ok=true, sample_rate, channels, samples (interleaved float), format="aiff"}`:
- 16-bit: `float(int16) * (1.0f / 32768.0f)` — **the identical expression the WAV reader uses**, so an AIFF
  and a WAV carrying the same samples produce bit-identical floats. `sowt` swaps the byte order.
- 24-bit (3 bytes/sample, sign-extended): `float(int32) * (1.0f / 8388608.0f)` (exact; a 24-bit sample whose
  low byte is 0 equals the 16-bit WAV sample of its top 16 bits). `sowt` is little-endian.
- Frame count = COMM `numSampleFrames` (extra bytes after the data are ignored).

### Call sites
- `src/engine/render.cpp` sampler loader (`render_sampler`): `read_wav(abs_path)` → `read_audio(abs_path)`.
  Everything after (error prefix `sampler: `, WP-0138 resampling, cache) is unchanged.
- `opd info` (`src/cli/main.cpp`, `cmd_info`): the audio branch is taken when the path ends in `.wav`,
  `.aif`, `.aiff` or `.aifc` (lowercase, as today for `.wav`), loads via `read_audio`, and prints
  `<format>: <path> rate=<n> ch=<n> frames=<n> duration=<x.xxx>s` where `<format>` is `rr.format` — exactly
  the existing line for WAV (`wav: …`), and `aiff: …` for AIFF/AIFF-C. The label follows the *content*, so
  an AIFF saved as `x.wav` prints `aiff:`. Do not change the usage text, and leave `opd analyze` on `read_wav`.

### Spec text — append to `docs/spec/project-model.md` as a new section
```markdown
## AIFF input — additive (WP-0158)
Sampler assets and `opd info` accept AIFF in addition to WAV. The format is chosen by the file's first
bytes, not its extension: `FORM` → AIFF reader, anything else → the WAV reader (behaviour and error
strings unchanged). Supported: AIFF and AIFF-C (`NONE`, `sowt`), 16- or 24-bit integer PCM, mono or
stereo, any sample rate 1–1000000 Hz that is an exact integer in the 80-bit extended field (the sampler
still accepts 8000–192000 Hz and resamples as for WAV). 16-bit samples convert with the same expression as
WAV, so an AIFF and a WAV with the same samples render byte-identically. Strict validation: error texts
`file too small for AIFF`, `not a FORM/AIFF file`, `COMM chunk too short`, `SSND chunk too short`,
`truncated sound data`, `missing COMM/SSND chunk`, `only mono/stereo supported`,
`unsupported AIFF-C compression`, `AIFF must be 16 or 24-bit`, `implausible sample rate`. `opd info`
reports them as `error: parse: <text>` (exit 4); the sampler reports `sampler: <text>` (render exit 5).
`opd info` prints `aiff: <path> rate=<n> ch=<n> frames=<n> duration=<x.xxx>s` for AIFF content.
```

## Acceptance criteria
- [ ] Only these files change: `src/engine/wav.hpp`, `src/engine/wav.cpp`, `src/engine/render.cpp`
      (the one call), `src/cli/main.cpp` (`cmd_info` only), `tests/test_main.cpp`, `docs/spec/project-model.md`,
      plus this task file.
- [ ] Unit tests in `tests/test_main.cpp` (a helper that builds AIFF bytes — including the 80-bit rate from an
      integer — in the test, written to `/tmp/op_aiff_*.aiff`): (a) 16-bit stereo and mono AIFF decode to
      exactly the expected floats and rate/channels/`format=="aiff"`; (b) 24-bit; (c) AIFF-C `NONE` and
      `sowt` (16 and 24); (d) rate decode exact for 8000, 11025, 22050, 44100, 48000, 88200, 96000, 192000;
      (e) one CHECK per error row 2–12 above asserting the exact error text; (f) `read_audio` dispatches
      by signature: AIFF bytes in `x.wav` and WAV bytes in `x.aiff` both load, and a WAV result is
      identical to `read_wav`; (g) odd-sized unknown chunk before `SSND` (pad byte) and non-zero SSND offset;
      (h) a seeded structural fuzz like `test_wav_fuzz` (Pcg32 seed `20261120`, 400 trials: truncations and
      1–5 byte flips of a valid AIFF seed; each must return without crash/hang; any `ok` result must have
      finite samples with |s| ≤ 1.0 and `samples.size() == frames × channels`; the loop must complete).
      At least 15 new `CHECK(` calls in total. Register the new test function in `main` like its neighbours.
- [ ] Existing WAV behaviour unchanged: `bash harness/run_goldens.sh` green, existing fixtures untouched.
- [ ] A project whose sampler asset is an AIFF renders byte-identically to the same samples as WAV; `opd info`
      works on AIFF files (verify by hand with a Python-generated file and show the output in the handoff).
- [ ] Spec section appended. All gates green: build with `-Werror` (0 warnings, `-j4` max), `op_tests`,
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`,
      and a `-DDZ_SANITIZE=ON` build of `op_tests` passes.

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row), existing fixtures under `tests/fixtures/goldens/`, `ai-sidecar/`,
  `docs/adr/`, `opd analyze`, the WAV writer, `read_wav`'s logic.
- AIFF writing, 8-bit/32-bit/float AIFF, other AIFF-C codecs, markers/loops/instrument chunks, MP3/FLAC/OGG.
- Pack/bundle/attach changes (bundles carry any file already).

## Git
- Branch `wp-0158-aiff-reader` from the current HEAD of this worktree (it is current `main` — do not check out, pull or merge anything). Commit: `WP-0158: AIFF / AIFF-C reader for sampler assets and opd info`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/wav.hpp`: `ReadResult::format` (default `"wav"`); declarations of `read_aiff` and `read_audio` with doc comments.
  - `src/engine/wav.cpp`: `read_aiff` (chunk walk, validation rows 1–12 in spec order, 80-bit rate via integer-only decode, 16/24-bit BE and `sowt` LE conversion using the WAV reader's expressions); `read_audio` (dispatch by the `FORM` signature). `read_wav` untouched.
  - `src/engine/render.cpp`: the one call, `read_wav(abs_path)` → `read_audio(abs_path)`.
  - `src/cli/main.cpp`: `cmd_info` routes `.wav/.aif/.aiff/.aifc` to `read_audio` and prints `rr.format` as the label; small static helper `has_audio_ext` added beside it. Usage text and `analyze` unchanged.
  - `tests/test_main.cpp`: `test_aiff()` (27 CHECKs, includes the seeded fuzz with Pcg32 seed 20261120, 400 trials), registered in `main` after `test_wav_fuzz`. AIFF bytes are built in-test; files go to `/tmp/op_aiff_*`.
  - `docs/spec/project-model.md`: "AIFF input — additive (WP-0158)" section appended verbatim from the task file.
  - Changed file list is exactly the spec's list (`git diff --stat`):
    ```
    docs/spec/project-model.md |  13 ++
    src/cli/main.cpp           |  19 ++-
    src/engine/render.cpp      |   2 +-
    src/engine/wav.cpp         | 137 +++++++++++++++++++
    src/engine/wav.hpp         |  14 ++
    tests/test_main.cpp        | 323 +++++++++++++++++++++++++++++++++++++++++++++
    ```
- **How verified:**
  - Build, `-Werror` (`cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`, `cmake --build build -j2 --target opd op_tests`): exit 0, no warnings.
  - `./build/op_tests` (exit 0). AIFF lines:
    ```
    PASS aiff.16bit_stereo_exact
    PASS aiff.16bit_mono_exact
    PASS aiff.24bit_stereo_exact
    PASS aiff.aifc_none_16
    PASS aiff.aifc_sowt_16
    PASS aiff.aifc_none_and_sowt_24
    PASS aiff.rate_decode_exact_common_rates
    PASS aiff.ext80_encoder_matches_spec_44100
    PASS aiff.err_cannot_open
    PASS aiff.err_file_too_small
    PASS aiff.err_not_form_aiff
    PASS aiff.err_comm_too_short
    PASS aiff.err_ssnd_too_short
    PASS aiff.err_truncated_offset
    PASS aiff.err_missing_chunk
    PASS aiff.err_channels
    PASS aiff.err_aifc_compression
    PASS aiff.err_sample_size
    PASS aiff.err_implausible_rate
    PASS aiff.err_truncated_sound_data
    PASS aiff.zero_frames_valid
    PASS aiff.dispatch_aiff_in_wav_name
    PASS aiff.dispatch_wav_write_ok
    PASS aiff.dispatch_wav_in_aiff_name_identical_to_read_wav
    PASS aiff.dispatch_unopenable_is_wav_error
    PASS aiff.odd_chunk_pad_and_ssnd_offset
    PASS aiff.random_fuzz_survives_and_sane
    ```
    27 AIFF PASS lines, 27 `CHECK(` calls in `test_aiff`. Full suite: `ALL PASS (0 failures)`, 547 PASS lines.
  - `bash harness/run_goldens.sh`: `run_goldens: 8 fixtures, 0 failures`; no fixture file modified.
  - `bash harness/test_cli_errors.sh`: `test_cli_errors: 0 failures`.
  - `bash harness/test_glue_scripts.sh` (with `UV_CACHE_DIR=<workspace>/.cache/uv`): `test_glue_scripts: 0 failures`.
  - `-DDZ_SANITIZE=ON` Debug build of `op_tests` (`build-san`): builds with no warnings; run prints `ALL PASS (0 failures)`, no ASAN/UBSAN reports.
  - Byte-identical sampler render (scratch copy of `tests/fixtures/goldens/resampled.opj`, kick samples re-encoded as AIFF with a Python script):
    ```
    IDENTICAL 44.1k mono (resampled path)    # kits/kick44k.wav vs kits/kick44k.aiff, cmp
    IDENTICAL 48k stereo                     # kits/kick.wav vs kits/kick.aiff, cmp
    deterministic rerun                      # same AIFF render twice, cmp
    ```
  - `opd info` on AIFF (Python-generated files, plus WAV for regression):
    ```
    aiff: kits/kick.aiff rate=48000 ch=2 frames=14400 duration=0.300s
    aiff: kits/kick44k.aiff rate=44100 ch=1 frames=15434 duration=0.350s
    wav: kits/kick.wav rate=48000 ch=2 frames=14400 duration=0.300s
    aiff: kits/kick_named.wav rate=48000 ch=2 frames=14400 duration=0.300s     # AIFF content saved as .wav
    wav: kits/wav_named.aiff rate=48000 ch=2 frames=14400 duration=0.300s      # WAV content saved as .aiff
    error: parse: missing COMM/SSND chunk   (exit 4)                            # AIFF cut to 20 bytes
    error: not_found: kits/nope.aiff        (exit 3)
    ```
- **PROPOSE / BLOCKED:** none. Notes: the `has_audio_ext` helper in `main.cpp` sits just above `cmd_info` (not inside it); it only replaces the inline `.wav` suffix test. Duplicate COMM/SSND chunks are skipped before any validation, per "later duplicates are ignored".
