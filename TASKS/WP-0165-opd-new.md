---
id: WP-0165
title: `opd new` — create a canonical new project (and the Document-free `op::new_project` factory)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1
human_review: none
---

## Context — why this exists
The prototype goal is "arrange a 3-minute beat with samples and one synth". Today a project can only be born from
`gen-testproject`, a MIDI file or hand-written JSON. The app's "New project" action and scripts need one canonical
starting point: a valid project with chosen tempo/length/tracks, whose bytes are fixed by the spec.

## Spec — normative
Append the section **"New project — additive (WP-0165)"** to `docs/spec/project-model.md`. The runner copies the
final text over the repo file; if it is already there, leave it unchanged. **Read it: it fixes the exact file
content, option rules, error strings and exit codes. Implement it literally.**

Engine API — add to `src/engine/model.hpp` (namespace `op`, next to `project_bytes`) and `src/engine/model.cpp`,
**exactly these names**:
```cpp
struct NewTrackSpec {
    std::string name;                     // non-empty (validated by the caller, e.g. opd new)
    VoiceKind kind = VoiceKind::Synth;
    std::string path;                     // sampler only: stored verbatim as voice.path
};
struct NewProjectSpec {
    double bpm = 120.0;                   // (0, 1000]
    int bars = 4;                         // 1..4096  -> Project::length_bars
    int beats_per_bar = 4;                // 1..32
    int sample_rate = 48000;              // 8000..192000
    std::vector<NewTrackSpec> tracks;     // empty = the single default track {"Synth", Synth, ""}
};
Project new_project(const NewProjectSpec& spec);
```
`new_project` is pure (no I/O, no clock, no randomness) and does not validate ranges (callers do). It builds the
`Project` struct directly (seed 42, format_version 0; per track i: a default `Track`, `Pattern{track=i}` with no
notes, `Clip{pattern=i, start_bar=0, repeats=1}`); it must NOT go through `Document`.

CLI — `src/cli/main.cpp`: new subcommand `new` (parse options in a loop; validate EVERYTHING before looking at the
output path; then if the output exists and no `--force` → exit 5 with the spec'd message and do not open the file
for writing; else `op::save_project`). Add the usage lines:
```
  opd new <out.opj> [--bpm X] [--bars N] [--beats-per-bar N] [--sample-rate SR]
          [--track NAME:KIND[:PATH]]... [--force]   (KIND = sine|synth|sampler|metronome)
```
Use the existing helpers where they fit (`fail(...)` maps code names to exit codes: `usage` -> 2, `write` -> 5;
`parse_decimal` exists for digit-only integers). Error text is part of the contract: copy it from the spec.

Expected bytes for the default invocation `opd new a.opj` (indent 2, keys in alphabetical order as `project_bytes` writes them, one trailing `\n`):
```json
{
  "arrangement": [
    {
      "pattern": 0,
      "repeats": 1,
      "start_bar": 0.0
    }
  ],
  "beats_per_bar": 4,
  "bpm": 120.0,
  "format_version": 0,
  "length_bars": 4.0,
  "patterns": [
    {
      "notes": [],
      "track": 0
    }
  ],
  "sample_rate": 48000,
  "seed": 42,
  "tracks": [
    {
      "gain_db": 0.0,
      "name": "Synth",
      "pan": 0.0,
      "voice": {
        "attack_ms": 5.0,
        "kind": "synth",
        "release_ms": 30.0
      }
    }
  ]
}
```
(Do not hand-format JSON: `op::project_bytes` produces this; the block is for you to compare against. If your
output differs, your `new_project` content is wrong, not the serializer.)

## Acceptance criteria
- [ ] `op::new_project` + structs as above; `opd new` as spec'd (all defaults, every error string, exit codes,
      overwrite rule, success line).
- [ ] The default output matches the bytes above exactly; the created project passes `opd info`, renders
      (`opd render` gives exactly `total_samples_for` frames of silence for the default project: 384000 frames),
      and accepts a journal through `opd apply` (e.g. `add_note` on pattern 0).
- [ ] Unit tests in `tests/test_main.cpp` (register a new `test_new_project()` in `main`; use a unique temp dir:
      `std::filesystem::temp_directory_path()` + a per-test name containing the pid — no fixed `/tmp/op_*`):
      default spec -> bytes above; a 3-track spec (sine, sampler "kits/kick.wav", metronome) -> `load_project`
      succeeds on `project_bytes`, `tracks.size()==3`, `patterns.size()==3`, `arrangement.size()==3`, pattern i
      references track i; `new_project` is deterministic (two calls equal bytes). At least 5 new CHECKs.
- [ ] `harness/test_cli_errors.sh`: add checks for `new` — no args -> 2; `--bars 0` -> 2 and no file created;
      second run without `--force` -> 5; with `--force` -> 0.
- [ ] Spec section present in `docs/spec/project-model.md`; 0 warnings (`-Werror`); `op_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh` green; sanitizer build
      of `op_tests` (`-DDZ_SANITIZE=ON`) green. Build with `-j4` max.

## Out of scope — do NOT touch
- `src/app/`, `src/doc/`, `docs/adr/`, `ai-sidecar/`, existing golden fixtures, `ROADMAP.md` (the elephant adds
  the row), any other subcommand's behaviour. No new files in `tests/fixtures/`.
- A GUI, templates beyond the above, creating sample files, checking that a sampler PATH exists.

## Git
- Branch `wp-0165-opd-new` from `main`. Commit: `WP-0165: opd new creates a canonical project (op::new_project)`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` / `src/engine/model.cpp`: added `op::NewTrackSpec`, `op::NewProjectSpec` and
    `Project op::new_project(const NewProjectSpec&)` exactly as specified — pure, Document-free, no validation;
    builds Project directly (format_version 0, seed 42, length_bars = bars as double) with per-track
    default Track + `Pattern{track=i, notes=[]}` + `Clip{pattern=i, start_bar=0, repeats=1}`; sampler path stored
    verbatim. Voice defaults in the struct already match the spec per kind.
  - `src/cli/main.cpp`: new subcommand `new` (`cmd_new`) — option loop validates everything left-to-right before the
    output path is examined; overwrite without `--force` → exit 5 with the spec message and no file opened; then
    `op::save_project`. Usage lines added verbatim. Forward-declared existing `parse_decimal`; reused `fail()`,
    `parse_bar_value` (finite full-parse for `--bpm`).
  - `tests/test_main.cpp`: `test_new_project()` registered in `main` — 6 new CHECKs (default bytes byte-exact vs the
    spec block; determinism; 3-track sine/sampler/metronome spec saved from `project_bytes` then `load_project` ok;
    tracks/patterns/arrangement sizes == 3; pattern i references track i with empty notes and clip wiring; fields +
    sampler path verbatim). Temp dir: `temp_directory_path()/op_new_project_wp0165_<pid>`.
  - `harness/test_cli_errors.sh`: `new` section — no args → 2; `--bars 0` → 2 with no file created; bad track kind → 2
    with no file; success writes non-empty file; second run without `--force` → 5 with the exact spec message; `--force` → 0.
  - `docs/spec/project-model.md`: runner-installed section, included unchanged (not reworded).
- **How verified:** (all commands run on this aarch64 rig, `-j4`, `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv` for the uv suites)
  - `-Werror` build, zero warnings: touched all changed TUs, `cmake --build build -j4 | grep -icE "warning|error"` → `0`.
  - Unit tests: `./build/op_tests` → `ALL PASS (0 failures)` incl. new checks:
    ```
    PASS new_project.default_bytes_exact
    PASS new_project.deterministic
    PASS new_project.three_track_loads
    PASS new_project.three_track_sizes
    PASS new_project.pattern_i_references_track_i
    PASS new_project.fields_and_sampler_path
    ALL PASS (0 failures)
    ```
  - Default bytes == spec block: `opd new a.opj` then `cmp a.opj exp.json` → `BYTES MATCH SPEC BLOCK`; stdout `new: a.opj tracks=1 bars=4 bpm=120`.
  - Passes info / renders exactly `total_samples_for` silent frames / accepts journal:
    ```
    project: a.opj bpm=120.00 bars=4.00 tracks=1 patterns=1 render=384000 frames (8.000s) seed=42
    rendered 384000 frames @48000 Hz -> out.wav (seed=42)      # python wave: frames 384000, all silence: True
    apply: 1 op(s) -> a2.opj                                    # add_note on pattern 0
    ```
  - Every error string + exit code from the spec checked by hand (each printed exactly `error: usage: <msg>`, exit 2, no file created):
    `new` → `new needs <out.opj>`; `--bpm abc|0|1001|inf`; `--bars 0|-1|4097`; `--beats-per-bar 0|33`;
    `--sample-rate 7999|192001`; `--track Drum` / `:sine` → `expected NAME:KIND[:PATH]`; `D:drumkit` → `unknown kind 'drumkit' (…)`;
    `K:sampler` and `K:sampler:` → `sampler needs a PATH`; `S:sine:kits/x.wav` → `PATH is only valid for sampler`;
    trailing `--bpm` → `missing value for --bpm`; `--wat` / second positional → `unknown arg '<arg>'`; 65 tracks →
    `too many tracks (max 64)` (exit 2, no file; 64 tracks succeeds). Validation-before-outputpath proven:
    `opd new existing.opj --bars 0` → exit 2 usage error, not 5.
  - Overwrite rule: existing file + no force → `[5] error: write: ov.opj already exists (use --force)`, original bytes
    untouched (`cmp` clean); `--force` → exit 0. Unwritable parent → `[5] error: write: cannot write 'nodir/deep/a.opj'`.
  - Harness suites:
    ```
    harness/test_cli_errors.sh → test_cli_errors: 0 failures   (incl. new_no_args/new_bars_zero(+no_output)/new_bad_track_kind
                                                                (+no_output)/new_ok(+written)/new_exists_refused/new_force)
    harness/run_goldens.sh     → run_goldens: 9 fixtures, 0 failures
    harness/test_glue_scripts.sh → test_glue_scripts: 0 failures
    ```
  - Sanitizers: `cmake -S . -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4` (no warn/error lines in output),
    `./build-san/op_tests` → `ALL PASS (0 failures)`.
- **Metrics:** +6 unit CHECKs, +9 harness checks; engine diff ~+35 lines, CLI ~+100 lines; no existing behaviour touched.
- **PROPOSE / BLOCKED:** none.

