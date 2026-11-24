---
id: WP-0215
title: Bundled generated content — stdlib generator writes a CC0 48 kHz mono drum kit to `assets/kits`, four `set_synth_params` presets to `assets/presets`, and a sha256 manifest; `op_bundled_tests` regenerates and compares byte-for-byte
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.75
human_review: none
touches: [cmake, new:tools/gen_bundled_kit.py, new:tests/test_bundled_assets.cpp, new:assets/manifest.json, new:assets/presets/synth_presets.json, new:assets/kits/kick.wav, new:assets/kits/snare.wav, new:assets/kits/clap.wav, new:assets/kits/hat_closed.wav, new:assets/kits/hat_open.wav, new:assets/kits/rim.wav, new:assets/kits/tom.wav, new:assets/kits/perc_cowbell.wav, new:assets/kits/perc_clave.wav]
---

## Context - why this exists

Phase 1 needs something to play with out of the box: a starter drum kit for sampler tracks and a few starter
synth patches. Shipping third-party samples is not allowed (licensing), so this package **synthesises all
bundled content in-repo** with a deterministic, stdlib-only Python generator and commits the outputs plus a
sha256 manifest. Everything generated is **generated, CC0**. No engine, CLI or spec behaviour changes: the kit
is ordinary 48 kHz PCM WAVs that the existing sampler voice / `opd analyze` already read, and the presets are
ordinary `set_synth_params` argument sets that the existing op already accepts.

Existing code this package reuses (read it first, do not reimplement or change it): the `set_synth_params`
operation and its argument rules in `docs/spec/operations.md` §Synth parameters operation; the parameter
ranges and defaults in `docs/spec/project-model.md` §Synth voice v1; `op::wav::read_wav` in
`src/engine/wav.hpp`; `op::apply_op` + `op::Project`/`op::Voice` in `src/engine/ops.hpp` / `src/engine/model.hpp`;
the stdlib-only Python style of `tools/render_bench.py`.

## Spec - normative (this task file is the contract)

### Files (exactly these; stage no others)
- `tools/gen_bundled_kit.py` (new) - the generator, **the only hand-written source file of this package**.
- `assets/kits/kick.wav`, `assets/kits/snare.wav`, `assets/kits/clap.wav`, `assets/kits/hat_closed.wav`,
  `assets/kits/hat_open.wav`, `assets/kits/rim.wav`, `assets/kits/tom.wav`, `assets/kits/perc_cowbell.wav`,
  `assets/kits/perc_clave.wav` (new) - **generated, never hand-edited**: run the generator (below) and stage
  what it wrote.
- `assets/presets/synth_presets.json` (new) - generated.
- `assets/manifest.json` (new) - generated.
- `tests/test_bundled_assets.cpp` (new) - own `main`, own `CHECK(cond, name)` printing `PASS <name>` /
  `FAIL <name>`; one per-process scratch dir (`std::filesystem::temp_directory_path()` + name containing the
  pid), never fixed `/tmp/op_*`; do NOT edit `tests/test_main.cpp`.
- `CMakeLists.txt` - purely additive, exactly these two blocks (no other line changes):
  after `add_test(NAME tracks COMMAND op_tracks_tests)` append

  ```cmake
  # WP-0215: bundled generated content (assets/kits + assets/presets) — regenerates the tree and compares byte-for-byte.
  add_executable(op_bundled_tests tests/test_bundled_assets.cpp)
  target_link_libraries(op_bundled_tests PRIVATE opengine)
  target_compile_definitions(op_bundled_tests PRIVATE OP_TEST_DATA="${CMAKE_SOURCE_DIR}/tests")
  add_test(NAME bundled COMMAND op_bundled_tests)
  ```

  and after `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227` append
  `list(APPEND DZ_OUR_TARGETS op_bundled_tests)  # WP-0215`.

### Generator CLI (exact)

```
python3 tools/gen_bundled_kit.py [--out DIR] [--seed N]
```

- Default `--out` is `<repo>/assets`, resolved from the script's own location (`dirname(realpath(__file__))/../assets`),
  so the command works from **any** cwd; never depend on cwd.
- Default `--seed` is `20240517`.
- It creates `<out>/kits` and `<out>/presets` if needed and writes **exactly 11 files**: the 9 kit WAVs,
  `presets/synth_presets.json`, `manifest.json`. It must not write, delete or modify anything else.
- Exit `0` on success and print one stdout line starting with `gen_bundled_kit: wrote `; argparse-style usage
  errors exit `2`.
- **Python 3.12 standard library only**: every import in the file must be a standard-library module (one of
  `sys.stdlib_module_names`) - no third-party package of any kind (numpy, scipy, anything else). Banned
  specifically: `subprocess`, `time` and `datetime` imports, file-mtime reads and any other wall clock (the
  manifest has **no** timestamp key).
- Determinism (ADR-0004): fixed seed, `random.Random(seed)` for noise, no wall clock, no environment or cwd
  dependence, no dict-order dependence (sorted keys; any fixed, stable layout is fine - the checks parse the
  JSON text and never compare its formatting). Two runs with the same seed into different directories must
  produce **byte-identical** files, which is also what makes the committed tree verifiable.

### Kit samples (exact file names, format and lengths)

| file under `assets/kits/` | length (s) | frames (`round(sec × 48000)`) | intended sound (DSP is yours) |
|---|---|---|---|
| `kick.wav` | 0.45 | 21600 | pitch-dropping sine body (~150→46 Hz) + short click |
| `snare.wav` | 0.35 | 16800 | tonal body + band-passed noise |
| `clap.wav` | 0.40 | 19200 | 3-4 staggered noise bursts + tail |
| `hat_closed.wav` | 0.08 | 3840 | short bright filtered noise + metallic ring |
| `hat_open.wav` | 0.50 | 24000 | same voice, long decay (must be longer than `hat_closed`) |
| `rim.wav` | 0.12 | 5760 | very short click + two high partials |
| `tom.wav` | 0.55 | 26400 | pitch-dropping sine body, long decay |
| `perc_cowbell.wav` | 0.30 | 14400 | two inharmonic partials (bell) |
| `perc_clave.wav` | 0.10 | 4800 | short band-passed click |

Every one of the 9 files, with **no exceptions**:
- canonical PCM WAV: `fmt` chunk `1` (PCM), **1 channel**, **16 bits**, **48000 Hz**, byte layout
  `RIFF <len> WAVE fmt  <16,1,1,48000,96000,2,16> data <len> <samples>` (little-endian, no extra chunks,
  44-byte header) - the reader in `src/engine/wav.hpp` must accept it unchanged.
- frame count exactly `round(sec × 48000)` from the table above.
- **fade-out to silence**: a linear ramp applied to the last `240` frames, multiplier for frame index `i`
  (0-based, `n` frames) is `(n - 1 - i) / 240`, so the **last frame is multiplied by 0** and written as the
  exact integer `0`. After the ramp, the maximum `|x|` over the last 8 frames must be `<= 0.05`.
- **no onset click**: a linear ramp applied to the first `48` frames, multiplier `i / 48`, so **frame 0 is
  written as the exact integer `0`**; the maximum `|x|` over frames 1..3 must be `<= 0.10`.
- both ramps are applied **before** normalisation (so the peak target survives) and the sound must start
  immediately: the maximum `|x|` over the first `480` frames (10 ms) must be `>= 0.20` - no leading silence.
- **peak-normalised below -1 dBFS**: after both ramps, scale the float signal so its maximum `|x|` is exactly
  `0.70` (linear) = **-3.10 dBFS**, i.e. always below the -1 dBFS ceiling (0.8913 linear); then quantise to
  signed 16-bit round-to-nearest. Read back as `int16 / 32768.0`, the peak must land in `[0.68, 0.71]`.
  Do **not** normalise to 1.0, 0.99, 0.5 or "leave it unnormalised".
- the 9 files are pairwise **byte-distinct** (no duplicated sample under two names), and each one is really
  **synthesised by the generator**: none of them may be a byte-copy of a file that already exists in the repo
  (in particular nothing under `tests/fixtures/`, whose `goldens/kits/*.wav` are fixtures, not kit content),
  and none may be silence.
- The synthesis itself (waveforms, envelopes, filters, noise, seeds per sound) is free; only the properties
  above and the exact byte-identity between the committed files and your own regeneration are checked.

### `assets/presets/synth_presets.json` (generated; exact content)

Top-level keys are exactly `format_version`, `generator`, `license`, `note`, `presets`, with
`"format_version": 0`, `"generator": "tools/gen_bundled_kit.py"`, `"license": "CC0-1.0"`,
`"note": "generated in-repo by tools/gen_bundled_kit.py; no third-party samples"` and `presets` an array of
**exactly four** objects **in this order**, each with exactly the keys `args`, `name`, `op` where `op` is the
string `set_synth_params` and `args` is deep-equal to:

```json
[
  {"name": "bass",  "op": "set_synth_params", "args": {"adsr": {"attack_s": 0.004, "decay_s": 0.14, "release_s": 0.08, "sustain": 0.65}, "cutoff_hz": 620.0, "resonance": 4.0, "waveform": "saw"}},
  {"name": "lead",  "op": "set_synth_params", "args": {"adsr": {"attack_s": 0.002, "decay_s": 0.2,  "release_s": 0.25, "sustain": 0.7},  "cutoff_hz": 4200.0, "resonance": 2.0, "waveform": "square"}},
  {"name": "pad",   "op": "set_synth_params", "args": {"adsr": {"attack_s": 0.6,   "decay_s": 0.8,  "release_s": 1.2,  "sustain": 0.8},  "cutoff_hz": 1400.0, "resonance": 0.9, "waveform": "saw"}},
  {"name": "pluck", "op": "set_synth_params", "args": {"adsr": {"attack_s": 0.001, "decay_s": 0.12, "release_s": 0.05, "sustain": 0.0}, "cutoff_hz": 2600.0, "resonance": 1.2, "waveform": "sine"}}
]
```

(JSON numbers may be written as `620` or `620.0`; equality is numeric.) Each `args` object therefore has keys
from `waveform`, `adsr`, `cutoff_hz`, `resonance` only, and each `adsr` object exactly
`attack_s`, `decay_s`, `release_s`, `sustain` - all inside the ranges of
`docs/spec/project-model.md` §Synth voice v1 (`waveform` in `sine|saw|square`, `cutoff_hz` in `[20, 20000]`,
`resonance` in `[0.5, 20]`, times in `[0, 10]`, `sustain` in `[0, 1]`), so
`{"op": "set_synth_params", "index": <track>, ...args}` is accepted by `op::apply_op` unchanged. Do not add
other preset names, extra keys, `index`, or a `null`.

### `assets/manifest.json` (generated; exact shape)

Top-level keys are exactly:

- `"format_version": 0`
- `"generator": "tools/gen_bundled_kit.py"`
- `"seed": <the integer seed used>` (the default `20240517` for the committed files)
- `"sample_rate": 48000`
- `"license": "CC0-1.0"`
- `"note": "generated in-repo by tools/gen_bundled_kit.py; no third-party samples"` - the licence note: the
  bundled content is generated in-repo, **CC0**, no third-party samples. This exact string also appears in
  `assets/presets/synth_presets.json`.
- `"files"`: an object mapping **every** generated path **relative to the output root, with `/` separators**
  (the 9 `kits/<name>.wav` entries plus `presets/synth_presets.json`) to its **lowercase hex sha256** of the
  file bytes. `manifest.json` is not listed in itself, and every file that exists under `assets/` except
  `manifest.json` **is** listed. No timestamp, no sizes, no extra keys.

### `tests/test_bundled_assets.cpp` (new target `op_bundled_tests`)

Runs `python3 <repo>/tools/gen_bundled_kit.py --out <scratch>/gen1` and a second time into `<scratch>/gen2`
(the repo root may be derived from `OP_TEST_DATA`, which is `<repo>/tests`), then asserts: the regenerated
file set equals the committed `assets/` tree and every file is **byte-identical** to its committed copy; the
two regenerations are byte-identical to each other; the manifest has exactly the keys above, `CC0-1.0`, the
licence note, `sample_rate` 48000, an integer `seed`, and lists exactly the files on disk (sha256 values are
64 lowercase hex chars); every kit WAV reads back through `op::wav::read_wav` as 1 channel / 48000 Hz with the
exact frame count from the table, a peak in `[0.68, 0.71]` (hence below -1 dBFS), first and last sample exactly
`0`, last-8-frame max `<= 0.05`, first-3-frames max `<= 0.10`, first-480-frame max `>= 0.20`; the 9 kit files
are pairwise distinct; the presets file has the exact shape and the four names `bass`, `lead`, `pad`, `pluck`
in that order with all values in range; and each preset applied to a one-synth-track `op::Project` through
`op::apply_op` succeeds and leaves the voice with those waveform / `cutoff_hz` / `resonance` / ADSR values.

### Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, fixed seed, byte-identical regeneration. The generator is Python; the
C++ test must build with `-Wall -Wextra -Werror` and pass under `-DDZ_SANITIZE=ON`. No new dependencies
anywhere.

## Acceptance criteria
- [x] `python3 tools/gen_bundled_kit.py` (run from the repo root, no arguments) writes the 11 files listed
      above, and running it a second time leaves the tree unchanged (`git status` clean apart from the new
      files); all 11 are committed.
- [x] Every kit WAV: 1 channel, 16-bit, 48000 Hz, exact frame count from the table, peak in `[0.68, 0.71]`
      (below -1 dBFS), first and last sample exactly `0`, last-8 max `<= 0.05`, frames 1..3 max `<= 0.10`,
      first-480 max `>= 0.20`, and 9 pairwise distinct files.
- [x] `assets/manifest.json` lists the 10 other generated files with correct sha256 sums; both JSONs carry
      `"license": "CC0-1.0"` and the licence note string verbatim.
- [x] For **each of the 9** kit files: `opd analyze assets/kits/<name>.wav` exits 0 and reports
      `"channels": 1`, `"sample_rate": 48000`, the `"frames"` value from the table and a `peak_dbfs` in
      `[-3.4, -2.9]`. For **each of the 4** presets: applying the single journal line
      `{"op":"set_synth_params","index":0, ...args}` (the preset's `args` verbatim, one preset per run) to a
      fresh project created with `opd new x.opj --track S:synth` exits 0 and the saved voice carries the
      preset's `waveform`.
- [x] `tests/test_bundled_assets.cpp` with at least 25 CHECKs covering: regeneration byte-for-byte (twice),
      manifest keys/licence/note/file list, per-kit format + length + peak headroom + silent ends + early
      attack, distinct kit files, preset shape/order/ranges, and `set_synth_params` acceptance of all four
      presets with the resulting voice fields.
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_bundled_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build
      (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_bundled_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (do not add or edit any row), `docs/` (including `docs/spec/` - no behaviour changes),
  `harness/`, `.github/`, `ai-sidecar/`, `src/` (no engine, CLI or app changes), `tests/test_main.cpp`,
  `tests/fixtures/` (existing goldens and `goldens/kits/*` are untouchable), `spikes/`, `third_party/`,
  `demos/`, `CMakeLists.txt` beyond the two additive blocks.
- No new `opd` subcommand, no new engine module, no project-format change, no changes to the sampler loader,
  no resampling, no third-party or downloaded samples, no model weights, no non-CC0 content, no `.aif`/24-bit
  variants, no extra kit sounds, no README or licence file added under `assets/`.

## Git
- Branch `wp-0215-bundled-generated-drum-kit` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0215: bundled generated CC0 drum kit and synth presets`. Stage explicit paths only (never `git add -A`):
  `tools/gen_bundled_kit.py`, `tests/test_bundled_assets.cpp`, `CMakeLists.txt`, the 9 `assets/kits/*.wav`,
  `assets/presets/synth_presets.json`, `assets/manifest.json`, and `TASKS/WP-0215-bundled-generated-drum-kit.md`
  (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the staged paths, nothing else)
- `tools/gen_bundled_kit.py` (new, 328 lines): the generator. Stdlib-only imports
  (`argparse, hashlib, json, math, os, random, struct, sys` — every one in `sys.stdlib_module_names`),
  no `subprocess`/`time`/`datetime`, no mtime reads (grep for `getmtime|st_mtime|os.utime|subprocess|import time|datetime`
  → exit 1, none). Default `--out` resolved from the script location (`normpath(dirname(realpath(__file__))/../assets)`,
  cwd-independent), default `--seed 20240517`. One `random.Random(seed)` stream, sounds in fixed order; biquad
  helpers (RBJ cookbook, DF1), closed-form swept sines, exponential envelopes. Ramps (48-frame onset `i/48`,
  240-frame fade-out `(n-1-i)/240`) applied before normalisation to exactly 0.70 linear, quantised round-to-nearest.
  Exactly three `open(...,"wb")` call sites → 9 + 1 + 1 = 11 files; prints `gen_bundled_kit: wrote 11 files to …`;
  argparse usage errors exit 2.
- `assets/kits/*.wav` (9), `assets/presets/synth_presets.json`, `assets/manifest.json` (new): generated by
  running the tool (304 KB total); never hand-edited. DSP per sound: kick/tom swept sines + click, snare
  two-part body + band-passed noise, clap 3 staggered bursts + tail, both hats = one `_hat` voice with short/long
  decay, rim click + two high partials, cowbell 560/1064 Hz inharmonic pair, clave band-passed click.
- `tests/test_bundled_assets.cpp` (new, 317 lines): own `main`, own `CHECK`, **129 CHECKs**; single per-process
  scratch dir `temp_directory_path()/op_bundled_tests_wp0215_<pid>` (removed at the end); runs
  `python3 <repo>/tools/gen_bundled_kit.py --out <scratch>/gen{1,2}` (repo root from `OP_TEST_DATA`), then:
  regen file-set == committed tree (11 exact names), per-file byte identity (11 CHECKs, gen1+gen2 vs committed),
  manifest 7 keys / CC0-1.0 / note verbatim / 48000 / integer seed / file-set match / 64-char lowercase-hex sums,
  per-kit `op::wav::read_wav` ok + mono/48k + exact frames + peak [0.68,0.71] + first/last sample exactly 0.0 +
  last-8 ≤ 0.05 + frames 1..3 ≤ 0.10 + first-480 ≥ 0.20 (8 CHECKs × 9), one pairwise-distinct CHECK over all
  36 pairs, preset shape/order/ranges, and `op::apply_op` on a one-synth-track `op::Project` for all four presets
  with `waveform`/`cutoff_hz`/`resonance`/ADSR field comparisons on the resulting `op::Voice`.
- `CMakeLists.txt`: **+7 / −0** (verified `git diff CMakeLists.txt | grep -c "^-[^-]"` → 0) — the exact two
  blocks from the task: the `op_bundled_tests` block after `add_test(NAME tracks …)` and
  `list(APPEND DZ_OUR_TARGETS op_bundled_tests)  # WP-0215` after the WP-0227 line.
- `TASKS/WP-0215-bundled-generated-drum-kit.md`: acceptance boxes ticked + this handoff. No other file touched
  (`git status`: only the staged paths below; `docs/`, `harness/`, `src/`, `tests/test_main.cpp`, `tests/fixtures/`
  all untouched — `git status --short` for them is empty).

### Determinism (ADR-0004)
```
$ python3 tools/gen_bundled_kit.py                       # from repo root
gen_bundled_kit: wrote 11 files to /home/shubhankar_dgx/qwen-scratch/openpattern/assets (seed 20240517)
$ cd /tmp && python3 <repo>/tools/gen_bundled_kit.py --out <ws>/tmp-wp0215/gen2   # cwd-independent ✓
$ (from repo root) … --out <ws>/tmp-wp0215/gen1
$ diff -r tmp-wp0215/gen1 tmp-wp0215/gen2   →  gen1==gen2 byte-identical
$ diff -r assets tmp-wp0215/gen1            →  committed==gen1 byte-identical
$ python3 tools/gen_bundled_kit.py ; python3 tools/gen_bundled_kit.py ; sha256sum assets/** > hash2
$ cmp hash1 hash2                            →  TREE UNCHANGED after two more runs
$ python3 tools/gen_bundled_kit.py --bad-usage >/dev/null 2>&1; echo $?   →  usage exit=2
```

### Kit properties — real output (stdlib reader, before C++ gates)
```
clap.wav         frames= 19200 peak=0.70001 peak_dbfs=-3.098  ok
hat_closed.wav   frames=  3840 peak=0.70001 peak_dbfs=-3.098  ok
hat_open.wav     frames= 24000 peak=0.70001 peak_dbfs=-3.098  ok
kick.wav         frames= 21600 peak=0.70001 peak_dbfs=-3.098  ok
perc_clave.wav   frames=  4800 peak=0.70001 peak_dbfs=-3.098  ok
perc_cowbell.wav frames= 14400 peak=0.70001 peak_dbfs=-3.098  ok
rim.wav          frames=  5760 peak=0.70001 peak_dbfs=-3.098  ok
snare.wav        frames= 16800 peak=0.70001 peak_dbfs=-3.098  ok
tom.wav          frames= 26400 peak=0.70001 peak_dbfs=-3.098  ok
FAILURES: 0     # incl. exact 44-byte header + data len, ends exactly 0, last8 ≤ 0.05,
                # frames 1..3 ≤ 0.10, first-480 ≥ 0.20, 36 pairwise-distinct pairs,
                # manifest keys/scalars/sets, all 10 sha256 recomputed == listed sums, preset shape/ranges
```
Manifest: keys exactly `files/format_version/generator/license/note/sample_rate/seed`, `seed=20240517`,
`sample_rate=48000`, `license="CC0-1.0"`, note verbatim, 10 entries (9 `kits/*.wav` + `presets/synth_presets.json`),
not self-listed; presets file top-level keys exactly `format_version/generator/license/note/presets`.

### Gates — real output
Build via dz-build skill (lock, `-j4`, ccache with `CCACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.ccache` because
`~/.cache/ccache` is read-only in this sandbox — the script's own fallback dir):
```
$ CCACHE_DIR=… bash …/dz_build.sh "$PWD" --out "$PWD/build-wp0215" --target opd --target op_tests --target op_bundled_tests
/home/shubhankar_dgx/qwen-scratch/openpattern/build-wp0215          # exit 0
$ grep -ciE warning build-wp0215/dz_build.log → 0                   # -Werror target set (DZ_OUR_TARGETS), 0 warnings
```
Tests:
```
$ ./build-wp0215/op_bundled_tests → 129 × PASS, "ALL PASS (0 failures)", exit 0
$ ./build-wp0215/op_tests         → "ALL PASS (0 failures)", exit 0
$ ctest -R "^bundled$" --output-on-failure (build-wp0215) → 100% tests passed, 0 tests failed out of 1   # Test #19: bundled Passed
```
Goldens — **no golden added or modified** (`git status --short tests/fixtures/` empty; 16 fixtures):
```
$ bash harness/run_goldens.sh build-wp0215/opd | tail -2
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures                               # exit 0
```
CLI contracts and glue-script e2e:
```
$ bash harness/test_cli_errors.sh build-wp0215/opd | tail -1
test_cli_errors: 0 failures                                        # exit 0
$ export UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv
$ bash harness/test_glue_scripts.sh        # with build/opd temporarily symlinked to build-wp0215/opd
21 × PASS … test_glue_scripts: 0 failures                          # exit 0   (symlink removed afterwards;
                                                                       # build/opd did not exist before and does not now)
```
Sanitizers (`-DDZ_SANITIZE=ON`, gcc 13.3.0, `DZ_BUILD_TYPE=Debug`):
```
$ DZ_BUILD_TYPE=Debug bash …/dz_build.sh "$PWD" --out "$PWD/build-san-wp0215" --sanitize --target op_tests --target op_bundled_tests   # exit 0
$ UBSAN_OPTIONS=print_stacktrace=1 ./build-san-wp0215/op_bundled_tests → ALL PASS (0 failures), exit 0
$ UBSAN_OPTIONS=print_stacktrace=1 ./build-san-wp0215/op_tests         → ALL PASS (0 failures), exit 0
$ grep -ciE "AddressSanitizer|runtime error|Direct leak" <both logs>   → 0 / 0
```

### Fixtures — acceptance output
`opd analyze assets/kits/<name>.wav` (fresh binary; default canonical JSON), one line per kit:
```
exit=0 kick.wav         channels=1 sample_rate=48000 frames= 21600 peak_dbfs=-3.098 -> OK
exit=0 snare.wav        channels=1 sample_rate=48000 frames= 16800 peak_dbfs=-3.098 -> OK
exit=0 clap.wav         channels=1 sample_rate=48000 frames= 19200 peak_dbfs=-3.098 -> OK
exit=0 hat_closed.wav   channels=1 sample_rate=48000 frames=  3840 peak_dbfs=-3.098 -> OK
exit=0 hat_open.wav     channels=1 sample_rate=48000 frames= 24000 peak_dbfs=-3.098 -> OK
exit=0 rim.wav          channels=1 sample_rate=48000 frames=  5760 peak_dbfs=-3.098 -> OK
exit=0 tom.wav          channels=1 sample_rate=48000 frames= 26400 peak_dbfs=-3.098 -> OK
exit=0 perc_cowbell.wav channels=1 sample_rate=48000 frames= 14400 peak_dbfs=-3.098 -> OK
exit=0 perc_clave.wav   channels=1 sample_rate=48000 frames=  4800 peak_dbfs=-3.098 -> OK
```
Per preset (one preset per run): `opd new <p>.opj --track S:synth` then one journal line
`{"op":"set_synth_params","index":0, …args}` applied with `opd apply <p>.opj <p>.opjl -o <p>_out.opj`;
both exit 0, saved voice carries the preset:
```
bass   new=0 apply=0 saved voice: waveform=saw    cutoff_hz=620.0  resonance=4.0 adsr={'attack_s': 0.004, 'decay_s': 0.14, 'release_s': 0.08, 'sustain': 0.65} -> OK
lead   new=0 apply=0 saved voice: waveform=square cutoff_hz=4200.0 resonance=2.0 adsr={'attack_s': 0.002, 'decay_s': 0.2,  'release_s': 0.25, 'sustain': 0.7}  -> OK
pad    new=0 apply=0 saved voice: waveform=saw    cutoff_hz=1400.0 resonance=0.9 adsr={'attack_s': 0.6,   'decay_s': 0.8,  'release_s': 1.2,  'sustain': 0.8}  -> OK
pluck  new=0 apply=0 saved voice: waveform=sine   cutoff_hz=2600.0 resonance=1.2 adsr={'attack_s': 0.001, 'decay_s': 0.12, 'release_s': 0.05, 'sustain': 0.0}  -> OK
```
Demo artifacts: the audio output itself is the committed bundle (the 9 WAVs above); no existing render or
behaviour changed, so no before/after pair applies — goldens staying byte-exact is the no-regression evidence.

### Follow-ups / notes
- No `PROPOSE:` items. Nothing outside the staged paths was touched.
- Pre-existing in this checkout, not committed: untracked `wpdraft6/` (another draft's worktree, present before
  this WP started) and gitignored `build/` (stale, contains no `opd`), `build-wp0215/`, `build-san-wp0215/`.

