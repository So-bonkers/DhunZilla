---
id: WP-0231
title: Sampler reverse playback — optional per-voice `reverse` key (canonical save omits it), undoable `set_sampler_reverse` op with `opd diff` support, new `sampler_reverse` golden
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
touches: [cmake, model, ops, render, new:tests/test_sampler_reverse.cpp, new:tests/fixtures/goldens/sampler_reverse.opj, new:tests/fixtures/goldens/sampler_reverse.wav]
---

## Context - why this exists
Producers reverse one-shots all the time: cymbal swells, opened-up reverb/pad tails, reversed rides under a
vocal. Phase 1's sampler already resamples an asset to the project rate (WP-0138), slices it with
`start_s`/`end_s` (WP-0156) and repitches it with `root_note` (WP-0167); what is missing is a per-voice
switch that plays that slice backwards. This package adds exactly that: one optional boolean key on a sampler
voice, one undoable operation that sets it, and `opd diff` support so the difference is expressible in a
journal. A voice without the key (every existing project) renders **byte-identically** — the 16 committed
goldens must not change by one byte.

Existing code this package reuses (read it first, do not reimplement or restructure it): `render_sampler`
(the resample cache, the trim indices `i0`/`i1` and `sampler_ratio`) in `src/engine/render.cpp`; `parse_voice`
and `voice_to_json` in `src/engine/model.cpp`; `apply_named` and `diff_projects` in `src/engine/ops.cpp`;
`op::Voice` in `src/engine/model.hpp`.

## Spec - normative (this task file is the contract)

### Files (exactly these; stage no others)
- `src/engine/model.hpp` - additive: inside `struct Voice`, immediately after the `root_note` member, add
  exactly this one member (name and type are fixed; other members untouched):
  `bool reverse = false;        // sampler: play the sliced buffer backwards (WP-0231); false = key omitted on save`
- `src/engine/model.cpp` - additive: parse the key in `parse_voice`, write it in `voice_to_json` (rules below).
- `src/engine/render.cpp` - additive: the reverse path inside `render_sampler` (§Render semantics below).
- `src/engine/ops.cpp` - new op branch in `apply_named` + `opd diff` support in `diff_projects`.
- `CMakeLists.txt` - purely additive, nothing else; exactly these lines after the line
  `add_test(NAME tracks COMMAND op_tracks_tests)`:

  ```cmake
  # WP-0231: sampler reverse playback — `reverse` key, set_sampler_reverse op, opd diff support; own test binary.
  add_executable(op_sampler_reverse_tests tests/test_sampler_reverse.cpp)
  target_link_libraries(op_sampler_reverse_tests PRIVATE opengine)
  add_test(NAME sampler_reverse COMMAND op_sampler_reverse_tests)
  ```

  and exactly this line after `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227`:

  ```cmake
  list(APPEND DZ_OUR_TARGETS op_sampler_reverse_tests)  # WP-0231
  ```
- `tests/test_sampler_reverse.cpp` (new) - own `main`, own `CHECK(cond, name)` printing `PASS <name>` /
  `FAIL <name>`, temp dir `std::filesystem::temp_directory_path() / ("op_sampler_reverse_" + pid)` (never a
  fixed `/tmp/op_*` path), removed at the end; `main` returns the failure count. Do NOT edit `tests/test_main.cpp`.
- `tests/fixtures/goldens/sampler_reverse.opj` + `tests/fixtures/goldens/sampler_reverse.wav` (new) - the
  golden (§Golden fixture). No other file under `tests/fixtures/` may change.

### Project JSON key `reverse`
- `{"kind": "sampler", "path": "kits/ramp48k.wav", "reverse": true}` — valid **only on sampler voices**, value
  a JSON boolean. Absent or `false` = forward playback.
- `op::Voice::reverse` is a `bool`, default `false`; a default-constructed `op::Voice` has `reverse == false`.
- Parse rules, checked in exactly this order (first failure wins; a parse failure is `opd render`/`opd apply`
  exit 4 with stderr `error: parse: <message>`):
  1. key present on a non-sampler voice → `voice.reverse is only valid for sampler voices`
     (this fires even when the value is also wrong, e.g. `"reverse": 1` on a sine voice);
  2. value present on a sampler voice and not a JSON boolean → `voice.reverse must be a boolean`
     (a number `1`/`0`/`1.0`, a string, `null`, an array or an object all give this one message).
  An explicit `"reverse": false` is **not** an error and behaves exactly like an absent key.
- Canonical save (`project_bytes` / `save_project`, and therefore `opd apply` output): write the key **only
  when true**, i.e. `if (v.reverse) j["reverse"] = true;`. Never write `"reverse": false`; a project whose
  voice has `reverse == false` must save byte-identically to one that never had the key.

### Render semantics — the order is normative
**resample → trim (`start_s`/`end_s`) → reverse → repitch.** Never reverse before the trim, never reverse
after the repitch, and never reverse the whole asset and then slice it.

Notation (all existing quantities, unchanged): after the resample cache and the trim, the voice plays buffer
frames `i0 .. i1-1`, `slice = i1 - i0`, and `want = llround(note.dur_beats * spb)`. Define the reversed slice
`R[k] = buffer[i1 - 1 - k]` for `0 <= k < slice` (mono; for a stereo asset both channels are reversed the same
way — `R` is per channel). With `reverse == false` the existing forward code runs unchanged. With
`reverse == true`:

- **no repitch** (absent `root_note`, absent note pitch, or `note == root_note`): output frame `n` plays
  `R[n]` for `n < min(slice, want)` — so a note shorter than the slice plays the slice's **last** frames
  first, and the slice's first frames are the ones left silent.
- **repitch** (`root_note >= 0`, note pitch present and `!= root_note`): the same linear-interpolation rule as
  the forward path, reading `R`: `pos = n * ratio` (per-frame multiply, never a running sum),
  `k = floor(pos)`, `frac = pos - k`, output = `float(double(R[k]) + frac * (double(R[k+1]) - double(R[k])))`,
  and `break` when `k + 1 >= slice` (a 1-frame slice repitched gives silence, exactly like forward).
  Interpolate between the two **reversed** neighbours `R[k]`, `R[k+1]` — never between forward frames.
- `reverse` is a voice property, not a note property: every note on that track is reversed; choke groups,
  mute/solo, velocity, pan, gains, clip fades, swing and the arrangement are untouched.
- Determinism (ADR-0004): the same project rendered twice gives identical bytes; no clock, no randomness, no
  fast-math, libm only via `engine/dsp.hpp`.

### Operation `set_sampler_reverse`
`{"op": "set_sampler_reverse", "index": <i>, "reverse": <true | false | null>}` — sets (or clears) the flag of
`tracks[index]`'s sampler voice. Follows the existing Apply semantics (strict keys, edit a copy, re-validate
through the model parse, commit atomically). Argument checks, in this order, error strings exactly
`set_sampler_reverse: <reason>` (exit 5, no output file written, project unchanged):
1. any key other than `op`, `index`, `reverse` → `unknown argument '<key>'`;
2. `index` with the existing shared `arg_index` rules → `missing argument 'index'`,
   `'index' must be an integer` (also for `0.0`), `'index' out of range`;
3. `reverse` missing → `missing argument 'reverse'`; present but neither a JSON boolean nor `null`
   (`1`, `1.0`, `"true"`) → `'reverse' must be a boolean or null`;
4. the track's voice is not a sampler → `track is not a sampler voice` (also for `"reverse": null`);
5. the edit writes `voice.reverse = reverse`, or **removes** the key for `null`; then the model parse above
   re-validates. `"reverse": false` clears the flag and the canonical save omits the key, so the saved file is
   byte-identical to a never-reversed project.

Undo/redo: no new mechanism — the existing `op::History` whole-project snapshots; a rejected apply leaves the
project and both stacks unchanged.

Exact examples of the strings (stderr of `opd apply` on a journal line, one line each):

```
set_sampler_reverse: unknown argument 'x'          # {"op":"set_sampler_reverse","index":0,"reverse":true,"x":1}
set_sampler_reverse: missing argument 'reverse'    # {"op":"set_sampler_reverse","index":0}
set_sampler_reverse: 'reverse' must be a boolean or null    # ..."reverse":1 / "true" / 1.0
set_sampler_reverse: 'index' out of range          # ..."index":5 on a one-track project
set_sampler_reverse: 'index' must be an integer    # ..."index":0.0
set_sampler_reverse: track is not a sampler voice  # a sine/synth/metronome track
```

### `opd diff` support
- The "voice differences are unsupported" check now compares the canonical voices with **only** the
  `reverse` key removed: a pair of tracks whose voices differ in anything else still fails with exit 5 and
  `error: diff: unsupported difference: tracks[<i>].voice` (e.g. a `root_note` difference, even when both
  sides are reversed); a pair differing **only** in `reverse` is supported.
- Emission: inside step 5 of the fixed order, immediately **after** `set_choke_group`, when a's and b's
  `reverse` differ (absent counts as `false`): exactly one op carrying b's value, i.e. `true` when b has the
  key, `false` when b lacks it (never `null`). The journal is `nlohmann::json::dump()` (keys sorted), so the
  line is byte-exactly:

  ```
  {"index":0,"op":"set_sampler_reverse","reverse":true}
  ```

  and `{"index":0,"op":"set_sampler_reverse","reverse":false}` for the direction that clears it.
- Every other step and its order are unchanged; `set_sampler_reverse` is the **last** op emitted for a track
  (after `rename_track`, `set_track_gain`, `set_track_pan`, `set_track_mute`, `set_track_solo`,
  `set_choke_group`). Journals of projects that never had the key are byte-identical to before.
- No new CLI command or flag: `opd apply` and `opd diff` already exist, `src/cli/main.cpp` is not touched.

### Golden fixture (new): `sampler_reverse`
`tests/fixtures/goldens/sampler_reverse.opj` — exactly this content (canonical JSON, indent 2, one trailing
newline; it is the canonical save of itself):

```json
{
  "beats_per_bar": 4,
  "bpm": 120.0,
  "format_version": 0,
  "length_bars": 1.0,
  "patterns": [
    {
      "notes": [
        {
          "dur": 0.5,
          "note": 67,
          "t": 0.0
        },
        {
          "dur": 0.5,
          "t": 1.0
        }
      ],
      "track": 0
    }
  ],
  "sample_rate": 48000,
  "seed": 7,
  "tracks": [
    {
      "gain_db": 0.0,
      "name": "ReverseRamp",
      "pan": 0.0,
      "voice": {
        "end_s": 0.075,
        "kind": "sampler",
        "path": "kits/ramp48k.wav",
        "reverse": true,
        "root_note": 60,
        "start_s": 0.025
      }
    }
  ]
}
```

`tests/fixtures/goldens/sampler_reverse.wav` — generate it with the finished binary and commit it unmodified
(never hand-write, re-encode or trim it):

```
opd render tests/fixtures/goldens/sampler_reverse.opj -o tests/fixtures/goldens/sampler_reverse.wav
```

Expected render (96000 frames, 48 kHz stereo pcm16; asset `kits/ramp48k.wav` has frame value `5*k`): the trim
is buffer frames `1200..3599`; note 1 (`root_note` 60, pitch 67 → ratio `2^(7/12)`, reversed slice read at
`pos = n*ratio`) occupies frames `0..1601` and note 2 (no pitch → plain reversed slice) frames
`24000..26399`; both channels equal; everything else silent. Spot values (left channel): frame `0` = 12724,
frame `1` = 12719, frame `100` = 12195, frame `1000` = 7427, frame `1601` = 4243, frame `1602` = 0,
frame `24000` = 12724, frame `24001` = 12721, frame `26399` = 4243, frame `26400` = 0. A correct
implementation reproduces all of them within 2 LSB; `harness/run_goldens.sh` must report
`PASS sampler_reverse: deterministic + golden match` and keep all 16 existing goldens bit-identical.

### Tests
`tests/test_sampler_reverse.cpp` — at least 25 `CHECK`s, covering at minimum:
`op::Voice().reverse == false`; an absent key parses and `project_bytes` contains no `reverse`;
`"reverse": true` parses, saves `"reverse": true` and round-trips stably; `"reverse": false` parses and saves
byte-identically to the absent-key project; every rejected value gives exactly
`voice.reverse must be a boolean`; a non-sampler voice gives exactly
`voice.reverse is only valid for sampler voices` (including when its value is also wrong); rendering a
project with `reverse == false` is bit-identical to one without the key; whole-asset reverse is bit-exact
(`R[k]`); trim-then-reverse is bit-exact and is **not** the reverse-then-trim mapping; a short note takes the
slice tail; repitch `+12` (ratio exactly 2.0) after reverse reads slice frame `2n`; repitch `-12` (ratio
exactly 0.5) after reverse has exact endpoints at even output frames and interpolated values inside the
reversed pair at odd ones; `note == root_note` still reverses; both channels of a stereo asset reverse; two
renders are identical; the op sets/clears (`false` and `null` both restore the never-reversed bytes) and
rejects with the exact messages above leaving the project unchanged; `diff_projects` emits exactly the one
journal line quoted above (both directions) and nothing for equal projects, and still reports
`unsupported difference: tracks[0].voice` for a `root_note` difference; step-5 order ends with
`set_sampler_reverse`; `op::History` apply/undo/redo are byte-exact and a rejected apply changes nothing;
`apply_journal` rejects a bad line atomically.

## Acceptance criteria
- [x] Everything above implemented; the 16 existing goldens bit-identical and the new golden matches
      (`bash harness/run_goldens.sh`).
- [x] `opd render` of a project without `reverse` is byte-identical to the base build's output, and so is one
      with `"reverse": false`.
- [x] `opd apply` with `{"op":"set_sampler_reverse","index":0,"reverse":true}` then `opd render` equals the
      render of the hand-written project with `"reverse": true`; `"reverse": false` and `"reverse": null`
      restore the exact original canonical bytes.
- [x] `opd diff` between a non-reversed and a reversed project writes exactly
      `{"index":0,"op":"set_sampler_reverse","reverse":true}` and `opd apply` of that journal reproduces b's
      canonical bytes; the reverse direction writes `{"index":0,"op":"set_sampler_reverse","reverse":false}`;
      a voice difference in anything else still exits 5 with
      `error: diff: unsupported difference: tracks[0].voice`.
- [x] Parse/op error strings exactly as specified (exit 4 for parse, exit 5 for a rejected op, no output file
      written on failure).
- [x] `tests/test_sampler_reverse.cpp` with at least 25 CHECKs covering the list in §Tests.
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_sampler_reverse_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, and a sanitizer
      build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_sampler_reverse_tests`.

## Out of scope - do NOT touch
- `docs/spec/` — do not edit any spec file. This package is capped at 6 files and this task file is its
  normative contract; mirroring §Render semantics into `docs/spec/project-model.md` and the op into
  `docs/spec/operations.md` is a follow-up — note it as `PROPOSE:` in the handoff.
- `ROADMAP.md`, `harness/`, `.github/`, `ai-sidecar/`, `src/cli/`, `src/app/`, `src/doc/`,
  `tests/test_main.cpp`, and every existing file under `tests/fixtures/` (including
  `tests/fixtures/goldens/kits/`).
- `opd audition` (no reverse flag or option), `opd new` and `gen-testproject` (never write the key),
  `opd tracks`/`opd clips`/MIDI export/`pack`/`unpack`, live playback and `src/app/player*`.
- Synth/sine/metronome voices; per-note `reverse`; choke-group, mute/solo, velocity, fade or swing semantics;
  anything other than linear interpolation (no new resampling work); no new dependencies.

## Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, libm only via `engine/dsp.hpp`, no fast-math, `-ffp-contract=off`;
passes `-Werror` and the `-DDZ_SANITIZE=ON` build. `opd render` stays bit-exact run-to-run.

## Git
- Branch `wp-0231-sampler-reverse-playback` from the current HEAD (the integration base: do not switch to main first).
  Commit: `WP-0231: Sampler reverse playback — reverse key, set_sampler_reverse op, opd diff support`.
  Stage explicit paths only (never `git add -A`). Include `TASKS/WP-0231-sampler-reverse-playback.md`
  (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed

Exactly the seven staged paths from §Files (nothing else; `git status` before commit showed only these):

- `src/engine/model.hpp` — one member added inside `struct Voice` immediately after `root_note`:
  `bool reverse = false;` (default = key omitted on save).
- `src/engine/model.cpp` — `parse_voice`: the `reverse` block placed after the `root_note` block; rule order
  is the spec's (non-sampler key message wins first even when the value is also wrong; one boolean message
  for null/numbers/strings/arrays/objects). `voice_to_json`: `if (v.reverse) j["reverse"] = true;` —
  `false` is never written.
- `src/engine/render.cpp` — the reverse branch inside `render_sampler` inserted after the trim indices and
  `want`/`slice`, before the untouched forward repitch/plain loops: resample → trim → reverse → repitch.
  Reversed slice read through `R[k] = buffer[i1 - 1 - k]` (per channel, no copy); repitch uses the same
  per-frame `pos = n * ratio` / floor / `break` on `k + 1 >= slice` rule, interpolating only between the
  reversed neighbours.
- `src/engine/ops.cpp` — `apply_named`: `set_sampler_reverse` branch (strict keys → `arg_index` →
  `reverse` present/type (boolean or null) → sampler check, in that order; `null` removes the key,
  `false` is written and normalized away by the canonical save). `diff_projects`: the voice-equality gate
  now compares canonical voices with the `reverse` key removed on both sides; emission is immediately after
  `set_choke_group` inside step 5, one op carrying b's value (`true`/`false`, never null).
- `CMakeLists.txt` — exactly the two specified additive blocks (test binary + `DZ_OUR_TARGETS` line).
- `tests/test_sampler_reverse.cpp` (new) — own `main`/`CHECK`, temp dir
  `temp_directory_path()/("op_sampler_reverse_" + pid)` removed at exit, returns failure count.
- `tests/fixtures/goldens/sampler_reverse.opj` (new) — byte-identical to the JSON block in this file
  (extracted programmatically, 631 bytes); verified to be the canonical save of itself
  (`opd apply sampler_reverse.opj empty.opjl -o x.opj && cmp` → byte-identical).
- `tests/fixtures/goldens/sampler_reverse.wav` (new) — generated with the finished binary, unmodified
  (command below). No other file under `tests/fixtures/` touched.

### How verified (commands + real output)

Build (dz_build skill script, Release, -j4, shared lock; the log contains 0 warnings, `-Werror` on):

```
bash .../dz_build.sh <worktree> --target opd --target op_tests --target op_sampler_reverse_tests
BUILD EXIT: 0            # grep -iE "warning:" <dir>/dz_build.log → 0 hits
```

`op_sampler_reverse_tests` — 35 CHECKs (≥25 required), all pass, exit 0:

```
PASS model.voice_default_false
PASS parse.absent_key_no_reverse_in_save
PASS parse.true_saves_and_round_trips
PASS parse.false_omitted_and_byte_identical
PASS parse.every_non_boolean_exact_bool_msg
PASS parse.non_sampler_key_rule_wins_first
PASS parse.reverse_coexists_with_root_and_trim
PASS render.whole_asset_reverse_bit_exact
PASS render.trim_then_reverse_bit_exact
PASS render.trim_before_reverse_not_reverse_before_trim
PASS render.short_note_takes_slice_tail
PASS render.repitch_plus12_reads_frame_2n
PASS render.repitch_minus12_endpoints_and_reversed_interp
PASS render.note_eq_root_still_reverses
PASS render.stereo_both_channels_reverse
PASS render.reverse_false_bit_identical_to_absent
PASS render.deterministic_two_renders
PASS op.set_true_matches_handwritten_project
PASS op.false_and_null_restore_never_reversed_bytes
PASS op.reject_unknown_argument
PASS op.reject_missing_reverse
PASS op.reject_reverse_not_boolean_or_null
PASS op.reject_index_out_of_range
PASS op.reject_index_float
PASS op.reject_non_sampler_including_null
PASS op.check_order_keys_before_index
PASS op.check_order_index_before_reverse
PASS diff.emits_true_line_and_reproduces_b
PASS diff.emits_false_line_and_reproduces_a
PASS diff.equal_projects_emit_nothing
PASS diff.root_note_difference_still_unsupported
PASS diff.step5_order_ends_with_set_sampler_reverse
PASS history.undo_redo_byte_exact
PASS history.rejected_apply_changes_nothing
PASS journal.bad_line_rejected_atomically
ALL PASS
```

Golden generated, spot values vs the §Golden fixture expectations — every one exact (0 LSB, tolerance is 2):

```
$ build/opd render tests/fixtures/goldens/sampler_reverse.opj -o tests/fixtures/goldens/sampler_reverse.wav
rendered 96000 frames @48000 Hz -> tests/fixtures/goldens/sampler_reverse.wav (seed=7)
frame 0: L=12724 R=12724 want=12724 dL=0 OK      frame 1: L=12719 ... want=12719 dL=0 OK
frame 100: L=12195 ... dL=0 OK                   frame 1000: L=7427 ... dL=0 OK
frame 1601: L=4243 ... dL=0 OK                   frame 1602: L=0 ... dL=0 OK
frame 24000: L=12724 ... dL=0 OK                 frame 24001: L=12721 ... dL=0 OK
frame 26399: L=4243 ... dL=0 OK                  frame 26400: L=0 ... dL=0 OK
all frames L==R: True / silent 1603..23999: True / silent 26401..end: True
SPOT VALUES PASS
```

`harness/run_goldens.sh` — every fixture bit-exact + golden match (18 fixtures = 17 pre-existing + new;
the task text says 16 — all pre-existing committed goldens are byte-unchanged and byte-exact, none regressed):

```
PASS sampler_reverse: deterministic + golden match
run_goldens: 18 fixtures, 0 failures
```
(other 17 lines `PASS <name>: deterministic + golden match` + `PASS negative-control`.)

Other standard gates:

```
$ build/op_tests                              → ALL PASS (0 failures), exit 0
$ bash harness/test_cli_errors.sh build/opd   → test_cli_errors: 0 failures
$ bash harness/test_glue_scripts.sh           → test_glue_scripts: 0 failures   (build/opd symlinked per skill, removed after)
$ ctest --test-dir <builddir> -R sampler_reverse → 100% tests passed, 0 tests failed out of 1
```

Sanitizers (`-DDZ_SANITIZE=ON`, ASAN+UBSAN, CI job's exact options `detect_leaks=1:abort_on_error=1` /
`print_stacktrace=1:halt_on_error=1`): build 0 warnings, both suites green:

```
$ dz_build.sh --sanitize --target op_tests --target op_sampler_reverse_tests   → BUILD EXIT: 0 (0 warnings)
op_sampler_reverse_tests → ALL PASS (exit 0)
op_tests                 → ALL PASS (0 failures) (exit 0)
```

Note: the sanitizer build must use `DZ_BUILD_TYPE=Debug` (what `.github/workflows/ci.yml` does:
`cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`). With the dz_build default (`Release`) gcc 13.3
emits a `-Werror=array-bounds` false positive at `render.cpp:910` (`out.arrangement = {clip};`, WP-0193 code
untouched by this WP). Verified pre-existing: the pristine base tree fails the Release+sanitize build with the
identical error. See PROPOSE below.

CLI acceptance (opd from the Release build):

```
$ opd apply base.opj <{"op":"set_sampler_reverse","index":0,"reverse":true}> -o applied.opj
$ opd render applied.opj -o a.wav && opd render handwritten_rev.opj -o h.wav && cmp a.wav h.wav
AC3a: apply true -> render == hand-written render (byte-identical)
AC3b: reverse:false restores exact original bytes        (cmp back_false.opj base.opj → identical)
AC3c: reverse:null restores exact original bytes         (cmp back_null.opj base.opj → identical)
$ opd diff base.opj handwritten_rev.opj -o edits.opjl; od -c edits.opjl
{"index":0,"op":"set_sampler_reverse","reverse":true}\n          (byte-exact)
$ opd apply base.opj edits.opjl -o repro.opj && cmp repro.opj handwritten_rev.opj
diff-apply: reproduces b's canonical bytes
$ opd diff handwritten_rev.opj base.opj -o edits2.opjl
{"index":0,"op":"set_sampler_reverse","reverse":false}\n         (byte-exact)
$ opd diff handwritten_rev.opj other_voice.opj -o bad.opjl; echo $?
error: diff: unsupported difference: tracks[0].voice   → exit 5
$ opd apply base.opj <"reverse":1> -o should_not_exist.opj; echo $?
error: apply: line 1: set_sampler_reverse: 'reverse' must be a boolean or null → exit 5, no output file
$ opd render bad_parse.opj -o bad.wav; echo $?
error: parse: voice.reverse must be a boolean → exit 4, no output file
$ opd render base.opj -o b.wav && opd render false_key.opj -o f.wav && cmp b.wav f.wav
AC: render 'reverse':false == render without key (byte-identical)
```

Bit-identity vs the base build's render output: the 17 committed goldens (rendered by the base build into
`*.wav` fixtures) all match bit-exactly against this build — that is `run_goldens`'s 0 failures above;
the pre-existing goldens and `kits/` are untouched in `git status`.

### Metrics

- Files: 5 modified + 3 new (opj/wav/test) + this task file. Interfaces touched: model, ops, render — within scope.
- New test binary `op_sampler_reverse_tests`: 35 CHECKs, 0 failures, clean Release and Debug+ASAN/UBSAN runs.
- Golden spot values: 0 LSB off on all 10 quoted values.

### Demo artifact

`tests/fixtures/goldens/sampler_reverse.opj` + `tests/fixtures/goldens/sampler_reverse.wav` are the committed
before/after pair: rendering the same voice without the key reproduces the pre-WP forward render byte-exactly
(the pre-existing goldens prove it).

### Follow-ups / PROPOSE

- PROPOSE: mirror §Render semantics into `docs/spec/project-model.md` (a "Sampler reverse — additive (WP-0231)"
  section) and `set_sampler_reverse` into `docs/spec/operations.md` — explicitly out of scope here (6-file cap),
  the task file is the normative contract for now.
- PROPOSE: make the local sanitizer path hard to mis-hit — `dz_build.sh` defaults to `CMAKE_BUILD_TYPE=Release`,
  where gcc 13.3 raises the `array-bounds` false positive at `render.cpp:910`; the CI sanitizers job uses Debug.
  Either document `DZ_BUILD_TYPE=Debug --sanitize` in the dz-build skill or pin the sanitizer configure to Debug
  in the CMake (`if(DZ_SANITIZE)`). Base tree fails identically, so this is a toolchain/environment fix, not WP-0231.
- `opd audition` and the JUCE player intentionally know nothing about `reverse` (out of scope).
