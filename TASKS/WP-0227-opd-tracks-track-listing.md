---
id: WP-0227
title: `opd tracks <project>` — read-only channel-rack / mixer track listing (canonical JSON, the renderer's own mute/solo rule)
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.6
human_review: none
touches: [cmake, cli:main, new:src/engine/tracks.hpp, new:src/engine/tracks.cpp, new:tests/test_tracks.cpp, spec:operations]
---

## Context - why this exists
Phase 1 exit criterion is "arrange a beat in the playlist"; the channel-rack / mixer panel and the future
copilot must read track state (names, kinds, fader/pan, mute/solo, audibility, choke groups, voice
parameters) from ONE canonical source instead of parsing `.opj` JSON themselves. This package adds the
read-only view `opd tracks <proj.opj>`: one JSON entry per track **in model order**, with the computed
audibility taken from **the renderer's own** mixer rule — never a re-implementation — and each voice
serialized exactly as the canonical project file serializes it, so optional keys the model does not hold
are absent, never invented. It adds no model field, no operation, and changes no render: every existing
golden must stay bit-exact.

Existing code this package reuses (read it first, do NOT reimplement or modify it): `op::track_audible`
(`src/engine/model.hpp` — the single source of truth for the mixer mute/solo rule, spec §mixer),
`op::project_to_json` (`src/engine/model_json.hpp`, implemented in `src/engine/model.cpp` — its
`tracks[i]["voice"]` sub-object IS the canonical voice serialization with every omit-at-default rule),
`op::Project` / `op::Track` / `op::Voice` / `op::load_project` (`src/engine/model.hpp`). The CLI shape to
copy is `cmd_clips` (WP-0199) in `src/cli/main.cpp`.

## Spec - normative (this task file is the contract)
`docs/spec/operations.md` (new section "Channel rack / mixer track view — `opd tracks` — additive
(WP-0227)") is **already updated by the runner** before you start. Read it first; it is the contract and
wins on any difference. If that section is NOT in `docs/spec/operations.md`, write BLOCKED in the handoff
and stop; do not invent the contract and do not edit `docs/spec/` yourself. The section says exactly what
is below — no more, no less.

### Files (exactly these 5; stage no others)
- `src/engine/tracks.hpp` (new) - declares `op::tracks_json` with a doc comment (see Interface).
- `src/engine/tracks.cpp` (new) - the implementation.
- `tests/test_tracks.cpp` (new) - own `main`, own `CHECK(cond, name)` printing `PASS <name>` / `FAIL <name>`
  (copy the style of `tests/test_meters.cpp`), returns the failure count; project files it needs go in one
  per-process scratch dir (`std::filesystem::temp_directory_path()` + a name containing the pid), removed at
  the end — never fixed `/tmp/op_*` paths. Do NOT edit `tests/test_main.cpp`.
- `src/cli/main.cpp` - three purely ADDITIVE insertions (usage line, `static int cmd_tracks(...)`, dispatch
  line) plus one include line; modify/delete no existing line.
- `CMakeLists.txt` - purely additive: the exact lines and anchors below.

### Interface (exact signatures, CLI syntax, JSON keys, error strings, exit codes)

#### CLI
```
opd tracks <proj.opj>
```
Exactly one positional argument, no options. On success stdout is ONLY the JSON text described below and
nothing else, exit 0. Errors print NOTHING on stdout: missing/extra argument or any token starting with
`-` → usage error, exit 2, stderr line `error: usage: tracks needs exactly <project.opj>`; file missing →
exit 3 (`not_found`, detail = the path); project fails to parse → exit 4 (`parse`, detail = the loader's own
message). Read-only: the command never writes files and never modifies the project. Deterministic: same
input → byte-identical stdout. A project with an empty `tracks` array never reaches this view — the loader
rejects it first (exit 4), so `opd tracks` on a real file always prints at least one entry.

#### Engine API (namespace `op`, in NEW `src/engine/tracks.hpp`)
```cpp
std::string tracks_json(const Project& p);
```
Pure function returning EXACTLY the text `opd tracks` prints. No I/O, no clock, no randomness; two calls on
the same `Project` return identical bytes. A `Project` with no tracks (only constructible in memory) returns
exactly `"[]\n"`.

#### Output text (byte-exact contract)
A JSON array built with the vendored `nlohmann::json`, dumped as `arr.dump(2)` plus **exactly one trailing
`\n`** (two-space indent; object keys sort alphabetically automatically). Do NOT hand-format any number:
`index` and `choke_group` are JSON integers; `gain_db`, `pan`, `attack_ms`, `release_ms`, `freq_hz` are the
model's float32 values widened to double, so their decimal text is nlohmann's shortest round-trip of that
float (a project file holding `"pan": 0.4` prints `0.4000000059604645` — identical to what `op::project_bytes`
writes for the same track; that is correct, not a bug); `start_s`, `end_s`, `root_note` and the ADSR/filter
values follow the file format exactly.

Each entry has EXACTLY these ten keys and no others, ALWAYS present (defaults print as themselves —
`false`, `0.0`, `0`; unlike the project file this view does NOT omit default mixer fields):

| key | value |
|---|---|
| `index` | int, the track's position in `Project::tracks` (0-based) |
| `name` | string, stored `track.name` (JSON-escaped by nlohmann) |
| `kind` | string, the voice kind exactly as the project file writes it: `"sampler"`, `"synth"`, `"metronome"` or `"sine"` |
| `gain_db` | number, stored `track.gain_db`; always present (default prints `0.0`) |
| `pan` | number, stored `track.pan`; always present (default prints `0.0`) |
| `mute` | bool, stored value; always present (`false` when not muted) |
| `solo` | bool, stored value; always present (`false` when not soloed) |
| `audible` | bool = `op::track_audible(p, i)` — call that function; do NOT re-derive the rule. (With any track soloed, only non-muted solo tracks are audible; mute always wins over solo — a muted AND soloed track is NOT audible.) |
| `choke_group` | int, stored `track.choke_group`; always present (`0` = none) |
| `voice` | object, the voice exactly as the canonical project serializer writes it for this track — see below |

**The `voice` object (normative):** byte-identical to `tracks[i]["voice"]` in the canonical serialization of
the SAME project (`op::project_bytes(p)`), i.e. produced by the file format's own voice serializer, so
**optional keys the model does not hold are ABSENT — never invented as `null`, `0`, `-1`, `""` or defaults**:

- sampler: always `kind`, `path`; `start_s` only when non-zero (WP-0156), `end_s` only when a slice end is
  set, `root_note` only when a root note is set (WP-0167; an absent root note must NOT appear as `-1`).
- synth: always `kind`, `attack_ms`, `release_ms`; `waveform` (`"sine"`/`"saw"`/`"square"`) only when the
  voice is not legacy; `adsr` — with ALL four inner keys `attack_s`, `decay_s`, `sustain`, `release_s`
  (defaults `0.005`, `0.0`, `1.0`, `0.030`) — only when an ADSR was given; `cutoff_hz` only when non-zero;
  `resonance` only when different from its default `0.7071` (WP-0161 canonical save).
- metronome: exactly `kind`, `accent_every`. sine: exactly `kind`, `freq_hz`.

The SIMPLEST correct implementation of the voice object is to build it once with the public
`op::project_to_json(p)` and copy `tracks[i]["voice"]` into the entry — that reuses the file format's rules
instead of duplicating them (do not call `save_project`, do not touch `model.*`).

Byte-exact example — three tracks: `"Zeta Kick"` (sampler, gain −3.5 dB, pan 0.25, soloed, choke group 2,
path `kits/kick.wav`, trim 0.0625–0.5 s, root note 60), `"Alpha Lead"` (synth v1 saw + ADSR + cutoff 2500,
resonance at default → omitted; muted), `"Click"` (metronome, all defaults):

```json
[
  {
    "audible": true,
    "choke_group": 2,
    "gain_db": -3.5,
    "index": 0,
    "kind": "sampler",
    "mute": false,
    "name": "Zeta Kick",
    "pan": 0.25,
    "solo": true,
    "voice": {
      "end_s": 0.5,
      "kind": "sampler",
      "path": "kits/kick.wav",
      "root_note": 60,
      "start_s": 0.0625
    }
  },
  {
    "audible": false,
    "choke_group": 0,
    "gain_db": -6.0,
    "index": 1,
    "kind": "synth",
    "mute": true,
    "name": "Alpha Lead",
    "pan": -0.5,
    "solo": false,
    "voice": {
      "adsr": {
        "attack_s": 0.01,
        "decay_s": 0.25,
        "release_s": 0.125,
        "sustain": 0.5
      },
      "attack_ms": 5.0,
      "cutoff_hz": 2500.0,
      "kind": "synth",
      "release_ms": 30.0,
      "waveform": "saw"
    }
  },
  {
    "audible": false,
    "choke_group": 0,
    "gain_db": 0.0,
    "index": 2,
    "kind": "metronome",
    "mute": false,
    "name": "Click",
    "pan": 0.0,
    "solo": false,
    "voice": {
      "accent_every": 4,
      "kind": "metronome"
    }
  }
]
```

### Implementation guide (all edits purely ADDITIVE — modify/delete no existing line)
1. `src/engine/tracks.hpp` (new): header guard `#pragma once`, include `"engine/model.hpp"`, declare
   `std::string tracks_json(const Project& p);` inside `namespace op` with a doc comment stating the
   contract above (pure, dump(2)+`\n`, ten keys always present, voice verbatim from the canonical
   serializer, empty project → `"[]\n"`).
2. `src/engine/tracks.cpp` (new): include its own header, `<cstddef>`, `<string>`, `<utility>`,
   `<nlohmann/json.hpp>` and `"engine/model_json.hpp"`. Build `const nlohmann::json canonical = project_to_json(p);`
   once, then for each `i` in `[0, p.tracks.size())` emit the ten keys into an `nlohmann::json` object with
   `e["audible"] = track_audible(p, i);` and `e["voice"] = canonical.at("tracks")[i].at("voice");`, push
   into an array and return `arr.dump(2) + "\n"`.
3. `src/cli/main.cpp` — four additive insertions:
   - include line, immediately AFTER `#include "engine/sample_index.hpp"` (keeps the block alphabetical):
     `#include "engine/tracks.hpp"`
   - usage line, inserted immediately BEFORE the `opd steps` usage line and immediately AFTER the
     `opd clips` usage line, exactly:
     `        "  opd tracks <proj.opj>   (channel rack / mixer track listing as canonical JSON — read-only, model order)\n"`
   - a `static int cmd_tracks(int argc, char** argv)` function placed immediately BEFORE the comment line
     that starts `// Parse a decimal argument` (style: copy `cmd_clips` verbatim in shape — reject
     `argc != 2 || argv[1][0] == '-'` with `usage(); return fail("usage", "tracks needs exactly <project.opj>");`,
     then `fs::exists` → `fail("not_found", path)`, then `op::load_project` → `fail("parse", err)`, then
     print `op::tracks_json(p)` with `std::fwrite` and return 0);
   - dispatch line, inserted immediately AFTER `if (cmd == "clips") return cmd_clips(argc - 1, argv + 1);`
     and immediately BEFORE the `if (cmd == "steps")` line:
     `    if (cmd == "tracks") return cmd_tracks(argc - 1, argv + 1);`
4. `tests/test_tracks.cpp` — NEW file, own `main`, own `CHECK` macro (style of `tests/test_meters.cpp`),
   **≥ 20 CHECKs**, built Projects in code plus at least one save/load round through a scratch-dir `.opj`
   and the real binary via `OPD_BIN` for the CLI checks. Cover at least: empty-tracks Project → exactly
   `"[]\n"`; the byte-exact example above (all three entries); exactly one trailing `\n`; model order
   preserved with deliberately non-alphabetical names; `index` values `0..n-1`; audibility matrix — no solo
   → every un-muted track audible and a muted one not; one solo → only it audible; two solo of which one
   muted → only the unmuted solo (mute wins over your own solo); all tracks soloed; the ten entry keys all
   present at defaults (`"mute": false`, `"solo": false`, `"choke_group": 0`, `gain_db`/`pan` printing
   `0.0`); sampler voice with no trim/root note → those keys ABSENT (never `-1`), with them set → present;
   legacy synth voice → NO `waveform`/`adsr`/`cutoff_hz`/`resonance` keys, only `kind`/`attack_ms`/
   `release_ms`; synth v1 with ADSR + cutoff and default resonance → `resonance` ABSENT, everything else
   present; non-default resonance → present; metronome/sine report exactly their own keys; track name JSON
   escaping (quote/backslash/newline); two calls byte-identical; CLI: exit 0 stdout equals
   `tracks_json(project)` and the saved project file bytes unchanged + two runs byte-identical; CLI errors:
   no argument / extra argument / `--json` → exit 2 with empty stdout, missing file → exit 3, garbage file
   → exit 4.
5. `CMakeLists.txt` — immediately after the line `target_sources(opengine PRIVATE src/engine/audition.cpp)  # WP-0209` add:
   ```cmake
   target_sources(opengine PRIVATE src/engine/tracks.cpp)  # WP-0227
   ```
   immediately after `add_test(NAME audition COMMAND op_audition_tests)` add:
   ```cmake

   # WP-0227: opd tracks — channel rack / mixer track listing. Runs the opd binary, so it needs OPD_BIN.
   add_executable(op_tracks_tests tests/test_tracks.cpp)
   target_link_libraries(op_tracks_tests PRIVATE opengine)
   target_compile_definitions(op_tracks_tests PRIVATE OPD_BIN="$<TARGET_FILE:opd>")
   add_dependencies(op_tracks_tests opd)
   add_test(NAME tracks COMMAND op_tracks_tests)
   ```
   and immediately after `list(APPEND DZ_OUR_TARGETS op_clip_trim_midi_tests)  # WP-0193c` add
   `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227`.

### Determinism / repo rules (ADR-0004)
No wall clock, no unseeded randomness, libm only via dsp.hpp, no fast-math; passes `-Werror` and
`-DDZ_SANITIZE=ON`. The added lines must contain no `<chrono>`, `<random>`, `<cmath>`, `rand(` or
`std::sin/cos/pow/exp/log` — the view is a pure function of the model.

## Acceptance criteria
- [x] Everything above implemented; existing goldens bit-identical (`bash harness/run_goldens.sh`, NO golden
      added or modified).
- [x] `opd tracks tests/fixtures/goldens/mixer.opj` prints exactly 3 entries in model order — `"Click"`
      (metronome, soloed) with `"audible": true`; `"Bassline"` (sine, muted) and `"Pad"` (synth) with
      `"audible": false`; every entry carries all ten keys; nothing else on stdout; exit 0.
- [x] `opd tracks tests/fixtures/goldens/synth_v1.opj` reports the `"SawLead"` voice with `waveform`,
      `adsr` (all four inner keys), `cutoff_hz` and `resonance`; `"SquareBass"` without `resonance`; the
      legacy `"OldSynth"` track's voice has only `kind`, `attack_ms`, `release_ms`.
- [x] Audibility equals `op::track_audible` for every track of every project tested, including a muted AND
      soloed track under another track's solo (not audible) and the no-solo case.
- [x] Optional voice keys absent from the model never appear; always-present view keys (`mute`, `solo`,
      `choke_group`, `gain_db`, `pan`) always appear, defaults included.
- [x] Read-only & deterministic: project file bytes unchanged after running; two runs byte-identical; no
      files created anywhere.
- [x] Errors: exit 2 (usage, stdout empty, stderr contains `tracks needs exactly <project.opj>`), 3
      (missing file), 4 (parse error); no crash on any input.
- [x] `tests/test_tracks.cpp` with ≥ 20 CHECKs, all passing (`build/op_tracks_tests` exits 0).
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `op_tracks_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer
      build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_tracks_tests`.

## Out of scope - do NOT touch
- `ROADMAP.md` (the elephant adds the row), golden fixtures (`tests/fixtures/`, read-only, add none),
  `harness/`, `.github/`, `ai-sidecar/`, `docs/adr/`, `docs/spec/*` (already updated by the runner),
  `src/app`, `src/doc`, `tests/test_main.cpp`.
- `src/engine/model.*`, `src/engine/ops.cpp`, `src/engine/render.*` — no model field, no operation, no
  render change, no diff change. Read them; modify none.
- Any project-level keys in the output (`bpm`, `master_gain_db`, track counts, a wrapper object): the
  output is ONLY the array of per-track entries.
- Sorting/filtering options (`--sort`, `--track`, `--json` flag), meters/loudness values, pattern or clip
  information, stem routing, colors, and any name sanitization — none of that belongs here.
- Re-implementing the mute/solo rule anywhere: always call `op::track_audible`.
- Emitting `"[]\n"` from the CLI is impossible (the loader rejects empty tracks); do not special-case it.

## Git
- Branch `wp-0227-opd-tracks-track-listing` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0227: opd tracks — read-only channel rack / mixer track listing (canonical JSON, renderer's mute/solo rule)`. Stage explicit paths only (never `git add -A`). Include `TASKS/WP-0227-opd-tracks-track-listing.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed (exactly the 5 files + the runner-installed spec, nothing else)
- `src/engine/tracks.hpp` (new, 30 lines): declares `std::string tracks_json(const Project& p);` in `namespace op`
  with a doc comment stating the full contract (pure, `dump(2)` + one `\n`, ten keys always present, voice
  verbatim from the canonical serializer, empty project → `"[]\n"`).
- `src/engine/tracks.cpp` (new, 40 lines): builds `const nlohmann::json canonical = project_to_json(p);` once;
  per track emits the ten keys with `e["audible"] = track_audible(p, i);`, `e["kind"] = vj.at("kind");` and
  `e["voice"] = vj;` (`vj = canonical.at("tracks")[i].at("voice")`), returns `arr.dump(2) + "\n"`. No model
  field, no operation, no render change. Includes only: own header, `<cstddef>`, `<string>`, `<utility>`,
  `<nlohmann/json.hpp>`, `"engine/model_json.hpp"` — no `<chrono>`/`<random>`/`<cmath>`/`rand(`/libm calls.
- `src/cli/main.cpp`: **+23 / −0** (verified: `git diff … | grep -c "^-[^-]"` → 0). Four additive insertions at
  the specified anchors: include line after `engine/sample_index.hpp`, usage line between the `opd clips` and
  `opd steps` lines, `static int cmd_tracks(int argc, char** argv)` (shape copied from `cmd_clips`) immediately
  before the `// Parse a decimal argument` comment, dispatch line between the `clips` and `steps` dispatch lines.
- `CMakeLists.txt`: **+9 / −0** — `target_sources(opengine PRIVATE src/engine/tracks.cpp)  # WP-0227` after the
  audition line; the `op_tracks_tests` block (executable, link opengine, `OPD_BIN`, `add_dependencies`,
  `add_test(NAME tracks …)`) right after `add_test(NAME audition COMMAND op_audition_tests)`;
  `list(APPEND DZ_OUR_TARGETS op_tracks_tests)  # WP-0227` after the WP-0193c line.
- `tests/test_tracks.cpp` (new, 526 lines): own `main`, own `CHECK` macro (style of `test_meters.cpp`), returns
  failure count; **38 CHECKs**; all projects/scratch files live in one per-process dir
  `temp_directory_path()/op_tracks_tests_wp0227_<pid>` removed at the end; CLI checks run the real binary via
  `OPD_BIN`. Covers every listed area: `"[]\n"`, byte-exact spec example, single trailing `\n`, model order +
  index values, full audibility matrix (no-solo / one-solo / two-solo-with-muted / all-soloed) plus an explicit
  "equals `op::track_audible` on every matrix project" check, ten keys at defaults, sampler optional-key
  absence (never `-1`) and presence when set, legacy synth exact key triple, synth v1 default-resonance omission,
  ADSR four inner keys, non-default resonance, metronome/sine exact voices, name escaping (`"` `\` newline,
  round-trip via parse), two-call byte-identity, save/load round trip, CLI stdout == `tracks_json`, file bytes
  unchanged, two runs identical, no files created (dedicated subdir entry-count), and all five error contracts.
- `docs/spec/operations.md`: the runner-installed WP-0227 section, committed **unchanged**.

### Gates — real output
Build via dz-build skill (lock + ccache + `-j4`, unique dirs; never raw cmake/make):
```
$ bash …/dz_build.sh "$PWD" --out "$PWD/build-wp0227" --target opd --target op_tests --target op_tracks_tests   # exit 0
BUILD_DIR=/home/shubhankar_dgx/qwen-scratch/openpattern/build-wp0227
$ grep -ci warning build-wp0227/dz_build.log
0
```
Unit tests (`build/op_tracks_tests` here is `build-wp0227/op_tracks_tests`):
```
$ ./build-wp0227/op_tracks_tests | tail -1 ; echo exit $?
ALL PASS (0 failures)          # 38 × "PASS …" lines, exit 0
$ ./build-wp0227/op_tests | tail -1 ; echo exit $?
ALL PASS (0 failures)          # exit 0
$ ctest -R "^tracks$" --output-on-failure   (in build-wp0227)
1/1 Test #18: tracks ........... Passed      → 100% tests passed, 0 tests failed out of 1
```
Goldens — **no golden added or modified** (`git diff tests/fixtures/` empty; still 16 `.wav` goldens):
```
$ bash harness/run_goldens.sh build-wp0227/opd | tail -2
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures
```
CLI error contracts and glue-script e2e:
```
$ bash harness/test_cli_errors.sh build-wp0227/opd
… 84 × PASS …
test_cli_errors: 0 failures                                              # exit 0
$ export UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv
$ bash harness/test_glue_scripts.sh        # with build/opd temporarily symlinked to build-wp0227/opd (dz-build skill); original restored after
… 21 × PASS …
test_glue_scripts: 0 failures                                            # exit 0
```
Sanitizers (`-DDZ_SANITIZE=ON`, gcc 13.3.0; config note in Environment notes below):
```
$ DZ_BUILD_TYPE=Debug bash …/dz_build.sh "$PWD" --out "$PWD/build-san-wp0227v2" --sanitize --target op_tests --target op_tracks_tests   # exit 0
$ UBSAN_OPTIONS=print_stacktrace=1 ./build-san-wp0227v2/op_tracks_tests ; echo exit $?   → ALL PASS (0 failures), exit 0
$ UBSAN_OPTIONS=print_stacktrace=1 ./build-san-wp0227v2/op_tests          ; echo exit $?   → ALL PASS (0 failures), exit 0
$ grep -ciE "AddressSanitizer|runtime error|Direct leak" <both logs>                        → 0 / 0
```

### Fixtures — acceptance output
`opd tracks tests/fixtures/goldens/mixer.opj` → exit 0, stdout ONLY the JSON below (3 entries in model order;
all ten keys on every entry):
```json
[
  { "audible": true,  "choke_group": 0, "gain_db": -6.0, "index": 0, "kind": "metronome",
    "mute": false, "name": "Click",    "pan": 0.0,  "solo": true,
    "voice": { "accent_every": 4, "kind": "metronome" } },
  { "audible": false, "choke_group": 0, "gain_db": -4.0, "index": 1, "kind": "sine",
    "mute": true,  "name": "Bassline", "pan": 0.25, "solo": false,
    "voice": { "freq_hz": 55.0, "kind": "sine" } },
  { "audible": false, "choke_group": 0, "gain_db": -8.0, "index": 2, "kind": "synth",
    "mute": false, "name": "Pad",      "pan": -0.25, "solo": false,
    "voice": { "attack_ms": 40.0, "kind": "synth", "release_ms": 300.0 } }
]
```
(entries shown one-line here for readability; actual output is the full `dump(2)` text — same bytes as the
byte-exact unit test asserts per-key.)

`opd tracks tests/fixtures/goldens/synth_v1.opj` → exit 0; key check (per-entry keys == exactly the ten, and
voice keys):
```
SawLead    -> True ['adsr', 'attack_ms', 'cutoff_hz', 'kind', 'release_ms', 'resonance', 'waveform']
SquareBass -> True ['adsr', 'attack_ms', 'cutoff_hz', 'kind', 'release_ms', 'waveform']            # no resonance (file default)
SineBell   -> True ['adsr', 'attack_ms', 'kind', 'release_ms', 'waveform']
OldSynth   -> True ['attack_ms', 'kind', 'release_ms']                                             # legacy: only the three
```
`adsr` carries all four inner keys on every v1 track (`attack_s`, `decay_s`, `sustain`, `release_s`).

### Manual checks (read-only, determinism, errors)
```
$ ./build-wp0227/opd tracks                       # stderr: error: usage: tracks needs exactly <project.opj> ; stdout empty; exit 2
usage exit: 2
$ ./build-wp0227/opd tracks tests/fixtures/goldens/mixer.opj > t1.json && ./build-wp0227/opd tracks …/mixer.opj > t2.json && cmp t1 t2
two runs byte-identical
$ md5sum tests/fixtures/goldens/mixer.opj ; git status --short tests/fixtures/     # unchanged, clean
6099f5217cda3df46e57a05457f9bfc9  tests/fixtures/goldens/mixer.opj
```
Exit 3 (missing file) and exit 4 (garbage file) verified with stderr detail (`not_found: <path>` / `parse: …`)
and empty stdout by unit checks `tracks.cli_missing_file_exit_3_detail_is_path` and `tracks.cli_garbage_file_exit_4_parse`.

### Environment notes (pre-existing, NOT caused by this WP — nothing here was changed)
1. `harness/run_goldens.sh` / `test_cli_errors.sh` default to `$ROOT/build/opd`, which in this checkout is a
   stale pre-existing binary (mtime Nov 23 04:33, not built from this tree). The first blind run showed
   `FAIL clip_trim`; with this WP's freshly built `build-wp0227/opd` all 16 goldens pass (render code untouched).
   For the glue scripts the skill requires `build/opd`: symlinked to the fresh binary, removed afterwards,
   original file restored byte-for-byte.
   PROPOSE: have both harness scripts accept/require an explicit fresh-build opd path by default (or an `$OPD` env var) so stale binaries can't skew gates.
2. Sanitizer gate config: `dz_build.sh --sanitize` now defaults `CMAKE_BUILD_TYPE=Release`; on gcc 13.3.0
   (`13.3.0-6ubuntu2~24.04.1`) ASAN + `-O3` emits a false-positive `-Werror=array-bounds` in **pre-existing**
   `src/engine/render.cpp:786` (`out.arrangement = {clip};`, WP-0192 code — out of scope here, not modified).
   The last known-good sanitizer build in this repo (`build-san-wp0210`, Nov 22) used `CMAKE_BUILD_TYPE=Debug`;
   reproducing that via the script's own `DZ_BUILD_TYPE` knob gives a clean sanitizer build and both suites pass
   with zero reports (evidence above).
   PROPOSE: make `dz_build.sh --sanitize` default to `DZ_BUILD_TYPE=Debug` (kit-side, one-line) so the sanitizers gate is reproducible.

### Metrics & follow-ups
- Diff: +596 new lines (30 + 40 + 526), main.cpp +23/−0, CMakeLists.txt +9/−0; modified/deleted existing lines elsewhere: none.
- 38 unit CHECKs, all passing in normal and ASAN+UBSAN builds; 16/16 goldens bit-exact; 84 CLI-contract checks; 21 glue e2e checks; negative control green.
- No model field, no operation, no render/diff change; the view is a pure function of the model (two byte-identical calls asserted).
- Follow-ups: none inside this package; the two PROPOSE notes above are harness/kit-side only.

