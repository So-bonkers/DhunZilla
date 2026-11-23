---
id: WP-0206
title: Phase 1 mixer send buses - project `buses` (name/gain_db/mute), per-track post-fader `sends`, bus stems in render
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 1.0
human_review: none
touches: [model, ops, render, cli:main, spec:project-model, spec:operations]
---

## Context - why this exists

The mixer (WP-0136) can only set per-track gain/pan/mute/solo and one master fader; there is nowhere to send
several tracks into one shared sub-mix (a "reverb bus", a "drum bus"). This package adds the Phase 1 routing
layer: an optional project-level `buses` list (`name`, `gain_db`, `mute`), per-track **post-fader** `sends` in
dB, and summing of every bus into the master **before** `master_gain_db` and before the CLI limiter. Gain
routing only - no effects, no inserts, no bus-to-bus routing. Bus management and send levels are ordinary
undoable operations validated by the existing save→parse path, `opd diff` can express them, and every bus gets
its own stem in `opd render --stems-dir`. **A project without buses stays byte-identical** (file bytes AND
render bytes) - that is the regression rule for this whole package.

Existing code this package reuses (read it first, do not reimplement it): `Project` / `Track` /
`project_from_json` / `project_to_json` / `project_bytes` in `src/engine/model.{hpp,cpp}`; the op dispatch and
the strict argument helpers (`arg_index`, `has_number`, `has_bool`, `has_string`, `has_object`, `check_keys`)
and `diff_projects` + `field_or` in `src/engine/ops.cpp`; `render_subset` / `render_project` / `render_stems`
/ `MixTarget` / `track_audible` in `src/engine/render.cpp`; `op::dsp::db_to_lin` in `src/engine/dsp.hpp`
(the ONLY way to touch libm); `sanitize_track_name` in `src/cli/main.cpp`.

## Spec - normative (this task file is the contract)

The two spec sections are shipped as full replacements of the docs files - `docs/spec/project-model.md` with its
new section "Send buses and post-fader sends - additive (WP-0206)", and `docs/spec/operations.md` with its new
section "Send-bus operations - additive (WP-0206)"; the runner copies them over the repo files. Everything below
restates them completely - implement from this file.

### Files (exactly these five source files; stage no others)
- `src/engine/model.hpp` - additive: a new `Bus` struct, `std::vector<float> sends;` in `Track`,
  `std::vector<Bus> buses;` in `Project`. No other public signature changes.
- `src/engine/model.cpp` - additive: parse/validate `buses` and `track.sends`; write them canonically.
- `src/engine/ops.cpp` - additive: six new operations in `apply_named`, plus the `opd diff` emissions below.
- `src/engine/render.cpp` - additive: bus routing in the mix, a per-bus render mode, bus stems in
  `render_stems`. `render_subset` is file-local; give it whatever parameter you need (e.g. an `int bus_index`
  defaulting to `-1`). `render.hpp`, `render_stems`' and `render_project`' signatures do NOT change.
- `src/cli/main.cpp` - additive: only inside the `--stems-dir` loop of `cmd_render` (stem file naming). Do not
  touch `usage()`.

Plus two NEW golden fixtures (see "Golden"): `tests/fixtures/goldens/buses.opj`,
`tests/fixtures/goldens/buses.wav`.

Do NOT add a test binary and do NOT edit `CMakeLists.txt`, `tests/test_main.cpp`, any other file under
`docs/`, `harness/`, `.github/`, `ai-sidecar/`, `src/doc/`, the JUCE app sources, or `ROADMAP.md`. Verification is the
golden plus the exact black-box commands listed under Acceptance criteria.

### Model: JSON keys, validation, canonical save

```jsonc
{
  "buses": [ { "gain_db": -3.0, "mute": true, "name": "Fx" } ],          // optional, max 16 entries
  "tracks": [ { "gain_db": 0.0, "name": "Kick", "pan": 0.0,
                "sends": [-6.0, 0.0], "voice": { "kind": "metronome" } } ]
}
```

The canonical writer sorts object keys alphabetically (as today), so `buses` lands between `bpm` and
`format_version`, a bus's keys are `gain_db`, `mute`, `name`, and a track's `sends` lands between `pan` and
`voice`.

**`buses`** - optional array, parsed **before** the `tracks` array (a track's `sends` length is checked against
the bus count). Checks in this order, first failure wins, each with exactly this error string:

1. value is not an array -> `buses must be an array`
2. more than 16 entries -> `buses must have at most 16 entries`
3. an entry is not an object -> `bus must be an object`
4. an entry has a key other than `name`, `gain_db`, `mute` -> `bus has unknown key '<key>'`
5. `name` absent, not a string, or the empty string -> `bus.name must be a non-empty string`
6. `name` equals an earlier bus's name -> `bus name '<name>' is already in use`
7. `gain_db` out of `[-96, 24]` -> `bus.gain_db out of range [-96, 24]` (read it as a float like
   `track.gain_db`; NaN rejected by the negated compare; a mistyped value throws inside `project_from_json`
   exactly like a mistyped `track.gain_db` does today - do not add your own message)
8. `mute` present and not a boolean -> `bus.mute must be a boolean`

Defaults: `gain_db` 0.0, `mute` false. Buses are addressed by **index** everywhere; names are labels only and
are never looked up by the engine.

**`track.sends`** - optional array of numbers, parsed **last** inside a track (after the voice and after the
choke-group checks, so no existing error precedence moves). Checks in order:

1. value is not an array -> `track.sends must be an array`
2. more entries than the project has buses -> `track.sends has more entries than there are buses`
3. an entry is not a JSON number -> `track.sends entries must be numbers`
4. an entry out of `[-96, 24]` -> `track.sends entries out of range [-96, 24]`

`sends[i]` is the send level in dB to bus `i`. Absent `sends`, or an array shorter than the bus count, means
**0.0 dB for every missing slot** - pad the loaded vector to the bus count with `0.0f`. A send of exactly
`0.0` dB routes nothing.

**Canonical save (byte-identity rule).** Write `buses` only when the list is non-empty; inside a bus write
`gain_db` only when it is not exactly `0.0f`, and `mute` only when `true`. Write a track's `sends` only when
the project has at least one bus **and** some entry is not exactly `0.0f`; then write exactly one entry per bus
(padded with `0.0`). Consequence: every pre-WP-0206 project - and every track that sends nothing - saves
byte-for-byte unchanged, and `harness/run_goldens.sh` (16 existing fixtures) must stay green without touching
a fixture.

### Operations (`src/engine/ops.cpp`, undoable via the normal `apply_op` / `op::History` path)

Argument checking uses the existing helpers verbatim (strict integer indices - `2.0` is rejected; unknown keys
rejected by `check_keys`; every failure reported as `"<op>: <reason>"`). Against a project with **no** `buses`
key the bus count is 0, so any bus index is out of range.

| operation | arguments | effect |
| --- | --- | --- |
| `add_bus` | `bus` (object) | append to `buses`, creating the array when absent; name/range rules enforced by the re-parse only |
| `rename_bus` | `index` (int), `name` (string) | set `buses[index].name`; empty or duplicate names are refused by the re-parse |
| `remove_bus` | `index` (int) | erase `buses[index]`, then for every track whose `sends` array is longer than `index`, erase element `index` (arrays stay bus-aligned). No "in use" rejection - a send is routing, not ownership |
| `set_bus_gain` | `index` (int), `gain_db` (number) | set `buses[index].gain_db`; range from the re-parse |
| `set_bus_mute` | `index` (int), `mute` (boolean) | set `buses[index].mute` (`false` is stored; the canonical save omits it) |
| `set_track_send` | `index` (track int), `bus` (int), `gain_db` (number) | pad `tracks[index].sends` with `0.0` up to the current bus count, then set element `bus`. Writing `0.0` is legal; if all entries end up zero the canonical save omits the key |

There is no bus-reorder operation (`add_bus` only appends). Do not add any other bus op.

### Render semantics (normative)

For each **audible** track `t` (`op::track_audible`; inaudible tracks never reach a bus) with the dry stereo
target gains it already uses, `L_t = 10^(gain_db/20) * panL`, `R_t = 10^(gain_db/20) * panR` - clip gain,
velocity, fades and choke included exactly as today:

```
bus_in_j  = sum over audible t of  voiceSample_t * L_t * 10^(sends[t][j]/20)      (same for R)
bus_out_j = bus_in_j * 10^(buses[j].gain_db/20)      if !buses[j].mute            else 0
mix       = ( sum over audible t of voiceSample_t * L_t ) + sum over j of bus_out_j   (buses in bus index order)
out       = mix * 10^(master_gain_db/20)
```

- Sends are **post-fader**: they scale the signal after the track's own `gain_db` and pan - moving a track's
  fader moves everything that track sends by the same factor.
- A send of exactly `0.0` dB is skipped entirely (that track adds nothing to that bus).
- A **muted** bus contributes nothing to the mix at all (not even its input sum).
- Buses are summed into the mix in **bus index order**, and this happens **before** the single master-gain
  multiply. The CLI limiter (`--limit-dbfs`) still runs after master gain, on the mix only - never on stems.
- When `p.buses` is empty nothing new is computed: every existing render stays bit-identical. Do not restructure
  the dry path (same `MixTarget` gains, same accumulation order). `10^(0/20) == 1.0f` exactly, so multiplying
  by a unity send must stay exact.

### Stems (`op::render_stems`, `opd render --stems-dir`)

`render_stems(p, base_dir, stems, mix, err)` keeps its signature and returns
`p.tracks.size() + p.buses.size()` buffers: the track stems in track order (unchanged), then **one stem per bus
in bus order**. A bus stem is exactly what that bus adds to the master, with `master_gain_db` applied - the same
convention as track stems (`bus_stem_j = bus_out_j * 10^(master_gain_db/20)`). A muted bus yields an all-zero
full-length stem. Track stems are NOT affected by buses: byte-identical with and without them, so with sends a
stem set is deliberately not additive (a track appears in its own stem and in every bus stem it feeds).

`opd render --stems-dir DIR` writes the bus stems after the track stems, continuing the numeric prefix:
`DIR/<i>_bus_<sanitized>.wav`, where `<i>` is the index into the whole stems list (track count + bus index) and
`<sanitized>` is the bus name through the existing `sanitize_track_name` (lower-case alphanumerics; spaces, `-`
and `_` become `_`); an empty result becomes `bus`. Track stem file names are unchanged. Example - two tracks
(`Kick`, `Pad`) and two buses (`Fx`, `Delay`):

```
stem 0 -> stems/0_kick.wav
stem 1 -> stems/1_pad.wav
stem 2 -> stems/2_bus_fx.wav
stem 3 -> stems/3_bus_delay.wav
rendered mix 192000 frames @48000 Hz -> mix.wav (+4 stems, seed=42)
```

`opd meters` keeps exactly one entry per track (bus stems are ignored there). `opd tracks`, `opd lint`,
`opd clips`, `opd click`, `opd info`, MIDI import/export and the player neither read nor write buses.

### `opd diff` additions

Nothing is added to the unsupported-difference list - bus differences are fully expressible. Two insertions into
the fixed emission order (existing steps keep their numbering; projects without buses produce **exactly the same
journal bytes** as before):

**Step 4c, immediately after `set_master_gain`** (before the loop-set op), comparing canonical `buses` arrays
with an absent key counting as the empty array:
1. `remove_bus` for indices `size(a.buses)-1` down to `size(b.buses)` (descending);
2. for bus `i = 0..` (`i < min(count_a, count_b)`), in this order, only where they differ: `rename_bus`
   (`name` = b's value), `set_bus_gain` (`gain_db` = b's value, absent counts as `0.0`), `set_bus_mute`
   (`mute` = b's value, absent counts as `false`);
3. `add_bus` for b's extra buses, ascending (`bus` = b's canonical bus JSON).

This block runs before any track operation so a track added later already carries a `sends` array within the
bus count that exists at that moment - every intermediate project must stay valid.

**Step 5 extension, after `set_choke_group`**, for each shared track: one `set_track_send` per bus slot
`j = 0..` (`j < size(b.buses)`, ascending) whose level differs, with `bus` = `j` and `gain_db` = b's value. An
absent `sends` array or a missing/short entry reads as `0.0`.

### Golden (new fixtures - this WP is explicitly authorised to ADD these two files only)

Build the project on top of the committed `demo` golden (three audible tracks: `Metronome`, `Bassline`, `Kick`)
so every routing path below carries real signal, then render with your own build. **Render from inside
`tests/fixtures/goldens/`** - that is where `kits/kick.wav` resolves for the sampler track:

```bash
cat > /tmp/wp0206.opjl <<'EOF'
{"op":"add_bus","bus":{"name":"Fx","gain_db":-3.0}}
{"op":"add_bus","bus":{"name":"Delay","gain_db":-6.0,"mute":true}}
{"op":"set_track_send","index":0,"bus":0,"gain_db":-6.0}
{"op":"set_track_send","index":1,"bus":1,"gain_db":-3.0}
{"op":"set_track_send","index":2,"bus":0,"gain_db":-12.0}
{"op":"set_master_gain","gain_db":-3.0}
EOF
cd tests/fixtures/goldens
opd apply demo.opj /tmp/wp0206.opjl -o buses.opj
opd render buses.opj -o buses.wav          # 192000 frames, seed 42, all three tracks audible
```

The saved fixture must therefore contain `"buses": [{"gain_db": -3.0, "name": "Fx"}, {"gain_db": -6.0, "mute": true,
"name": "Delay"}]`, `"master_gain_db": -3.0`, and track sends `[-6.0, 0.0]` (Metronome), `[0.0, -3.0]` (Bassline),
`[-12.0, 0.0]` (Kick). `harness/run_goldens.sh` then covers it: deterministic twice-run + bit-exact match against
the committed WAV (and the same harness must still pass the 16 existing fixtures untouched). Never modify an
existing fixture.

### Determinism / repo rules (ADR-0004)

No wall clock, no unseeded randomness, libm only via `op::dsp` wrappers (`db_to_lin`), no fast-math,
`-ffp-contract=off`; must pass `-Werror` and the `-DDZ_SANITIZE=ON` build. Fixed arithmetic order: tracks in
track order, then buses in bus index order, then one master-gain multiply.

## Acceptance criteria

- [x] Everything above implemented; `harness/run_goldens.sh` green with the 16 existing fixtures untouched plus
      the new `buses` pair (17 fixtures).
- [x] **Legacy byte-identity**: for a bus-less project, `opd apply <proj> <empty journal> -o out.opj` writes
      bytes identical to the input, and its rendered WAV is bit-identical to pre-WP-0206 (the existing goldens).
- [x] **Ops**: the six operations apply through `opd apply`; each of these is rejected with a non-zero exit and
      no output file written: duplicate bus name; empty/absent `bus.name`; unknown key inside a bus object;
      `gain_db` 24.5 or -97 on a bus; `mute` not a boolean; `remove_bus`/`set_bus_gain` with an out-of-range or
      non-integer (`0.0`) index; `set_track_send` when the project has no buses, with an out-of-range `bus`, or
      with `gain_db` outside `[-96, 24]`. Loading a hand-written project is refused for: `buses` not an array;
      17 buses; `track.sends` longer than the bus count; `sends` not an array; a non-numeric `sends` entry.
- [x] **Canonical save**: a bus at unity/unmuted saves without `gain_db`/`mute`; a track whose sends are all
      zero (including one set back to `0.0`) saves without `sends`; a shorter-than-bus-count array is padded with
      `0.0` on load and saved in full once any entry is non-zero; save→parse→save is byte-stable.
- [x] **Render** (check with `opd render --format f32` + stdlib Python on the samples; `lin(x) = 10**(x/20)`):
      with one track and one bus fed at -6 dB and unity bus gain, `mix == dry_mix * (1 + lin(-6))` sample-wise
      (relative tolerance 2e-4); with a bus gain of -6 dB, `mix == dry_mix * (1 + lin(-6)*lin(-6))`; the bus
      stem equals `track_stem * lin(send) * lin(bus_gain)`; lowering the track fader by 6 dB lowers that bus
      stem by `lin(-6)` too (post-fader); a muted bus leaves the mix unchanged within 1e-6 absolute and its stem
      is all zeros; track stems are byte-identical with and without buses.
- [x] **Stems CLI**: a project with 2 tracks + 2 buses writes exactly 4 stem files named as in the example
      above, and `opd meters` on it still prints one entry per track (no bus entries).
- [x] **Diff**: for each pair below, `opd diff a.opj b.opj -o e.opjl` then `opd apply a.opj e.opjl -o c.opj`
      yields `c.opj` byte-identical to `b.opj`: (a) no buses → one bus with two sends; (b) two buses → one bus
      (removal re-aligns the sends arrays); (c) bus renamed; (d) mute toggled and bus gain changed; (e) a send
      set back to exactly `0.0` (that slot emits nothing, and where it was the only non-zero send the journal is
      empty - zero-byte file); (f) buses plus an added track that sends to them. For two bus-less projects the
      journal contains no bus operation at all.
- [x] Standard gates: build (0 warnings, `-Werror`, `-j4`), `op_tests`, `harness/run_goldens.sh`,
      `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of
      `op_tests`.

## Out of scope - do NOT touch

- Effects/inserts on buses or tracks; panning a bus; sends from a bus to a bus; pre-fader sends; per-send mute.
- Bus reordering, and any new operation beyond the six listed.
- New CLI subcommands or flags; changes to `usage()`, `opd info`, `opd tracks`, `opd lint`, `opd meters`,
  `opd clips`, MIDI I/O, the player (the JUCE app sources) or the document layer (`src/doc/`).
- A new test binary: do NOT edit `CMakeLists.txt` or `tests/test_main.cpp`. (PROPOSE: a follow-up WP adds
  `tests/test_buses.cpp` with unit tests for `op::render_stems` bus buffers and the six ops.)
- Existing golden fixtures (`tests/fixtures/goldens/` other than the two new files), `harness/`, `.github/`,
  `ai-sidecar/`, `spikes/`, `docs/adr/`, `ROADMAP.md`, and every `docs/spec/*.md` except the two this WP ships
  as full replacements.

## Git
- Branch `wp-0206-mixer-send-buses` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0206: mixer send buses with post-fader sends and bus stems`.
  Stage explicit paths only (never `git add -A`). Include `TASKS/WP-0206-mixer-send-buses.md` (this task file)
  with the handoff filled.
- **Do not push or merge.**

---
## Handoff report - completed by the agent

### What changed

Five source files, exactly as scoped (no others staged):

- `src/engine/model.hpp` — new `Bus{name, gain_db, mute}` struct, `std::vector<float> sends;` in `Track`,
  `std::vector<Bus> buses;` in `Project`. No other public signature changes.
- `src/engine/model.cpp` — `project_from_json`: `buses` parsed before the `tracks` array (8 checks in the
  spec's order, exact error strings, `gain_db` read via `value(..., 0.0f)` so a mistyped value throws
  through the WP-0122 contract); `track.sends` parsed last inside a track (4 checks, padded to bus count).
  `project_to_json`: `buses` only when non-empty (per-bus `gain_db` ≠ 0.0f, `mute` only when true); a
  track's `sends` only when the project has buses AND some entry ≠ 0.0f, written one entry per bus.
- `src/engine/ops.cpp` — six ops in `apply_named` (`add_bus`, `rename_bus`, `remove_bus` incl. sends
  re-alignment, `set_bus_gain`, `set_bus_mute`, `set_track_send` with zero-padding), all through the
  unchanged copy→re-parse→re-validate path; `diff_projects`: step 4c right after `set_master_gain`
  (remove descending / rename,gain,mute / add ascending) and the step-5 `set_track_send` extension after
  `set_choke_group`. No new ruleset anywhere.
- `src/engine/render.cpp` — dry-loop body moved verbatim into a `render_track(ti, MixTarget&)` lambda
  (mix target parameterised — the dry path is byte-for-byte the old code); `render_subset` gains a
  file-local `int bus_index = -1`; mix sums each bus (audible tracks in track order into a bus buffer,
  bus gain, added in bus index order before the single master multiply; muted bus and 0.0-dB sends
  skipped entirely); per-bus stem mode renders exactly `bus_out_j` (bus gain applied in one final pass,
  muted bus → zero stem); `render_stems` returns `tracks + buses` buffers. `render.hpp` untouched.
- `src/cli/main.cpp` — only the `--stems-dir` naming loop: bus stems continue the numeric prefix as
  `<i>_bus_<sanitized>.wav` (empty sanitized name → `bus`), via the existing `sanitize_track_name`.
  `usage()` untouched.

New goldens (the only files ADDED under tests/): `tests/fixtures/goldens/buses.opj` + `buses.wav`,
generated exactly with the commands in §Golden from inside `tests/fixtures/goldens/`.
`docs/spec/project-model.md` + `docs/spec/operations.md` included as shipped by the runner, unchanged.

### How verified — real output

Build (dz-build kit, Release, `-Werror`, `-j4`): 0 warnings (`grep -c warning: dz_build.log` → `0`), dir
`/home/shubhankar_dgx/qwen-scratch/.builds/wp0206`.

Goldens, 16 existing fixtures untouched, before adding the new pair:

```
PASS arranged: deterministic + golden match
...
PASS time_sig_6_8: deterministic + golden match
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures
```

After generating `buses.opj`/`buses.wav` (rendered from inside `tests/fixtures/goldens/`):

```
rendered 192000 frames @48000 Hz -> buses.wav (seed=42)
run_goldens: 17 fixtures, 0 failures
```

Fixture content matches the spec verbatim:

```
buses: [{'gain_db': -3.0, 'name': 'Fx'}, {'gain_db': -6.0, 'mute': True, 'name': 'Delay'}]
master_gain_db: -3.0
Metronome sends= [-6.0, 0.0]
Bassline sends= [0.0, -3.0]
Kick sends= [-12.0, 0.0]
keys: ['beats_per_bar', 'bpm', 'buses', 'format_version', ...]   # buses between bpm and format_version
```

Legacy byte-identity (empty journal `opd apply` over bus-less projects; demo/mixer/sampler_choke identical
input→output) plus cross-binary proof that WP-0206 changed nothing — base build (d7e3fa3 worktree,
`.builds/wp0206-base`) vs new build on 6 fixtures:

```
PASS render_identical[demo] … PASS render_identical[sampler_choke]   (WAV cmp bit-exact)
PASS roundtrip_bytes_same[demo] … PASS roundtrip_bytes_same[sampler_choke]
```

Ops — every listed rejection exits non-zero with no output file (14/14):

```
PASS[dupname] PASS[emptyname] PASS[noname] PASS[unkkey] PASS[gain24_5] PASS[gain_97] PASS[mute_str]
PASS[rm_oob] PASS[rm_float] PASS[gain_float_idx] PASS[send_nobus] PASS[send_oob] PASS[send_gain] PASS[send_gain2]
   (all: rc=5, no output file)
```

Hand-written project loads refused with the exact spec strings:

```
PASS[buses_obj]: rc=4 error: parse: buses must be an array
PASS[17buses]: rc=4 error: parse: buses must have at most 16 entries
PASS[sends_long]: rc=4 error: parse: track.sends has more entries than there are buses
PASS[sends_obj]: rc=4 error: parse: track.sends must be an array
PASS[sends_str]: rc=4 error: parse: track.sends entries must be numbers
```

Canonical save:

```
PASS unity_bus_save [{'name': 'Plain'}]
PASS all_zero_sends_omitted; other track keeps [-6.0, 0.0]
PASS short_array_padded [-4.0, 0.0]
PASS zero_short_omitted
PASS canonical_stable            (buses.opj → s1 → s2 byte-stable)
```

Render (f32 renders + stdlib-Python over samples; rel = max relative error):

```
PASS mix == dry*(1+lin(-6)) rel=1.15e-07
PASS mix == dry*(1+lin(-6)*lin(-6)) rel=1.04e-07
PASS bus_stem == track_stem*lin(send)*lin(bus_gain) rel=2.93e-07
PASS post-fader: -6 dB track fader lowers bus stem by lin(-6) rel=2.29e-07
PASS muted bus: mix unchanged max_abs=0.00e+00
PASS muted bus stem all zeros
PASS track stem byte-identical with/without buses
PASS track stem byte-identical (muted-bus project)
```

Stems CLI + meters, exactly the spec's example (2 tracks `Kick`,`Pad`; 2 buses `Fx`,`Delay`):

```
stem 0 -> stems_out/0_kick.wav
stem 1 -> stems_out/1_pad.wav
stem 2 -> stems_out/2_bus_fx.wav
stem 3 -> stems_out/3_bus_delay.wav
rendered mix 96000 frames @48000 Hz -> mix2.wav (+4 stems, seed=42)
PASS meters_one_per_track: ['Kick', 'Pad'] master present
bus name "!!" → 2_bus_bus.wav          (empty sanitised result becomes "bus")
```

Diff (each pair: `opd diff a b -o e` then `opd apply a e -o c`, `cmp c b`):

```
PASS diff_roundtrip[a] (journal: 166 bytes)     no buses → one bus + two sends
PASS diff_roundtrip[b] (journal: 30 bytes)      two buses → one bus; {"op":"remove_bus"} re-aligns sends
PASS diff_roundtrip[c] (journal: 43 bytes)      bus renamed
PASS diff_roundtrip[d] (journal: 92 bytes)      mute toggled + bus gain changed
PASS diff_roundtrip[e] (journal: 0 bytes) + PASS e_journal_zero_bytes   send set back to 0.0, already-only-zero → empty
PASS diff_roundtrip[e2] + e2_kick_sends_key_gone                        [-12,0] → [0,0]: sends key dropped
PASS diff_roundtrip[f] (journal: 241 bytes)     buses + added track that sends to them
PASS no_bus_ops_in_journal                      two bus-less projects → journal has zero bus/send ops
bus name-swap pair → error: diff: unsupported difference: (journal does not reproduce target)   (safety net, as documented)
```

Order sanity: in (a) `add_bus` precedes `set_track_send`; in (f) `add_bus` precedes `add_track` whose
canonical object carries `sends` within the bus count valid at that moment.

Standard gates:

```
100% tests passed, 0 tests failed out of 18            (ctest, Release build)
test_cli_errors: 0 failures
test_glue_scripts: 0 failures                          (UV_CACHE_DIR=$HOME/qwen-scratch/.cache/uv; build/opd symlink removed after)
sanitizers: DZ_BUILD_TYPE=Debug -DDZ_SANITIZE=ON → build ok + 100% tests passed, 0 failed out of 18
```

### Metrics

- 460 insertions / 24 deletions across the 5 source files (see `git show --stat`); 2 fixtures added
  (buses.opj 3.2 KB, buses.wav 768 KB); 17/17 goldens green; 18/18 unit tests ×2 (Release + sanitizers).

### Follow-ups / notes

- PROPOSE: a follow-up WP adds `tests/test_buses.cpp` (unit tests for the six ops and the
  `op::render_stems` bus buffers) — explicitly out of scope here, as the WP itself suggests.
- PROPOSE: fixtures `arranged.opj`, `tempo.opj`, `synth_v1.opj` are committed in NON-canonical key
  order, so an empty-journal `opd apply` normalizes their bytes. Verified pre-existing: the base build
  from `d7e3fa3` round-trips them to the exact same bytes my build does (`FAIL(base)` identical for the
  same three fixtures; `PASS roundtrip_bytes_same[...]` for all six tested). Not touched by this WP.
- Note: the sanitizers gate is green with the CI recipe (`.github/workflows/ci.yml`:
  `cmake -B build-san -DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON`). A `-O2` Release + sanitizers combo
  trips a pre-existing gcc 13.3.0 `-Werror=array-bounds=` false positive in `pattern_mode_project`
  (`out.arrangement = {clip};`) — reproduced identically on the untouched `d7e3fa3` base, unrelated to
  this WP; not worked around here.
