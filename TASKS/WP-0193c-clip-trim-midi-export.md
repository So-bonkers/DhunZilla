---
id: WP-0193c
title: Clip trim in MIDI export — apply the offset/length window in `save_midi`'s placement loop so exported note events match render onsets
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [cmake, spec:project-model, src/engine/midi.cpp, "new:tests/test_clip_trim_midi.cpp"]
---

## Context — why this exists

WP-0193a (already in `main`) added the two optional clip keys `offset_beats` / `length_beats`, the
`set_clip_trim` operation and its `opd diff` emission. **MIDI export still ignores both keys**:
`op::save_midi` (`src/engine/midi.cpp`, the command `opd opj2mid <in.opj> -o <out.mid>`) has its own
placement loop (WP-0129, extent from WP-0162) that writes *every* note of *every* repeat at
`clip.start_bar·beats_per_bar + r·pattern_extent + n.t`. So a trimmed clip exports material the user
cut away and its onsets are in the wrong place. This package makes MIDI export honour exactly the
window `docs/spec/project-model.md` §Clip trim already defines — the same rule WP-0193b applies to the
renderer, so exported events match render onsets event-for-event.

This package is **independent of WP-0193b**: it changes only MIDI export. If your branch happens to be
on a `main` that already contains the render-side trim, nothing here changes; do not touch `render.cpp`.

Read first, do not change their contracts: `op::Clip` (`offset_beats`, `length_beats`) and
`op::pattern_extent()` in `src/engine/model.hpp`; the export section of `src/engine/midi.cpp`
(`save_midi`, its `Placement` struct, `kExportPpq = 480`, `sanitize_name`, `nearest_midi`);
`docs/spec/project-model.md` §Clip trim (WP-0193a — **normative**, quoted again below), §MIDI export
flattens arrangement (WP-0130), §Explicit pattern length, §MIDI export (`opd opj2mid`).

## Spec

### 1. The window (normative — identical to §Clip trim in `docs/spec/project-model.md`)

For each clip of `p.arrangement` whose pattern belongs to the track being written, with
`E = pattern_extent(pat)`, effective repeats `R = (E > 0 ? clip.repeats : 1)` (the existing rule),
`base = clip.start_bar * p.beats_per_bar`, `w0 = clip.offset_beats` and
`len = *clip.length_beats` when the key is present, else `R*E - w0`:

- The clip's **source** timeline is the unrolled pattern: note `n` of repeat `r` (0-based) sits at
  source position `s = r*E + n.t`.
- A note **plays** iff `w0 <= s < w0 + len` (lower bound inclusive, upper bound **exclusive**). Its
  events are placed at beat `base + (s - w0)`: note-on tick `llround((base + (s - w0)) * 480)`,
  note-off tick `llround((base + (s - w0) + n.dur_beats) * 480)`.
- Note durations are **not** shortened — only onsets are gated. A note that starts inside the window
  keeps its full `dur_beats` even if it rings past the window end.
- `len <= 0` ⇒ the clip contributes **no** note events at all (the track is still written, with its
  name meta and end-of-track; `save_midi` still returns `true`).
- A `length_beats` longer than the available material leaves silence: only the existing `R` repeats
  are scanned, extra repeats are never generated.
- An **untrimmed** clip (`offset_beats` absent or exactly `0` **and** `length_beats` absent) keeps the
  pre-WP-0193c path bit for bit: placement beat `clip.start_bar * beats_per_bar + r*E`, then
  `t_beats = n.t_beats + <placement beat>`. Every project without the keys — all committed goldens —
  must export **byte-identically**.
- Swing is not part of MIDI export (unchanged, spec §Pattern swing).

Worked example (use these numbers to check yourself): pattern notes `t = 0, 1, 2, 3` with `dur = 0.5`
⇒ `E = max(t + dur) = 3.5`. One clip `{start_bar: 0, repeats: 2, offset_beats: 2}` ⇒
`len = R*E - w0 = 7 - 2 = 5`, window `[2, 7)` over `s = 0,1,2,3, 3.5,4.5,5.5,6.5` ⇒ the notes at
`s = 2, 3, 3.5, 4.5, 5.5, 6.5` play, placed at beats `0, 1, 1.5, 2.5, 3.5, 4.5` (ticks
`0, 480, 720, 1200, 1680, 2160`). With the same clip and `length_beats: 2` the window is `[2, 4)` ⇒
only `s = 2, 3` play, at beats `0, 1`.

### 2. Shape of the change — `src/engine/midi.cpp`, inside `save_midi` only

Change only the per-track placement block (the local `struct Placement`, the two loops that fill it
and the note loop). Everything else in the file stays as it is: SMF format 1 at 480 ppq, track 0 =
tempo map + 4/4 meta, one track per op track in project order, name meta, velocity bake
`clamp(llround(n.velocity * gain_lin * 127), 1, 127)` with `gain_lin = clip.gain_db != 0 ? db_to_lin(clip.gain_db) : 1.0`,
the `(tick, off-before-on)` sort, non-negative delta clamping, the legacy no-arrangement path (every
pattern once at bar 0), `load_midi`, `import_midi_into` and `src/engine/midi.hpp`.

A placement carries everything both paths need — one entry per repeat. Reference shape (equivalent
code is fine; keep the names distinct as below):

```cpp
struct Placement {
    const Pattern* pat = nullptr;
    double offset_beats = 0.0;  // WP-0129 placement beat: clip start + r*E (untrimmed path)
    double base_beats = 0.0;    // WP-0193c clip start beat (start_bar * beats_per_bar)
    double src_base = 0.0;      // WP-0193c this repeat's source origin r*E
    double w0 = 0.0;            // WP-0193c trim offset (inclusive lower bound on s)
    double hi = 0.0;            // WP-0193c window end w0 + len (exclusive upper bound)
    bool trimmed = false;       // false → pre-WP-0193c arithmetic, bit for bit
    double gain_lin = 1.0;
};
```

Per clip: `proto.trimmed = (clip.offset_beats != 0.0) || clip.length_beats.has_value();`,
`proto.w0 = clip.offset_beats;`, `len = clip.length_beats ? *clip.length_beats : reps*extent - clip.offset_beats;`,
`proto.hi = clip.offset_beats + len;`; per repeat `r`: `src_base = r*extent`,
`offset_beats = base_beats + src_base`. Then, per note:

```cpp
if (!pl.trimmed) {
    t_beats = n.t_beats + pl.offset_beats;            // unchanged pre-WP-0193c placement
} else {
    const double s = pl.src_base + n.t_beats;         // source position in the unrolled pattern
    if (!(s >= pl.w0 && s < pl.hi)) continue;         // windowed out: no events at all
    t_beats = pl.base_beats + (s - pl.w0);            // spec placement beat base + (s - offset)
}
```

Naming trap (real, in this file and in `render.cpp`): `Placement::offset_beats` means the **placement
beat**, not the clip's trim offset. Do not give it a second meaning, do not rename it, and do not
reuse it for `clip.offset_beats` — keep trim values under their own names (`w0`, `hi`, `src_base`).

**Do not:** add files other than the test below; change `midi.hpp`, `load_midi`, `import_midi_into`,
`render.cpp`, `model.*`, `ops.cpp`, `src/cli/main.cpp`; add CLI flags, subcommands or error strings;
change velocities, tempo/time-signature meta, event ordering or tick rounding; touch
`tests/fixtures/goldens/`.

### 3. Tests — `tests/test_clip_trim_midi.cpp` (new file, own main)

Register it in `CMakeLists.txt` **additively** (no existing line edited or removed), next to the
existing `op_clip_trim_tests` block:

```cmake
add_executable(op_clip_trim_midi_tests tests/test_clip_trim_midi.cpp)
target_link_libraries(op_clip_trim_midi_tests PRIVATE opengine)
add_test(NAME clip_trim_midi COMMAND op_clip_trim_midi_tests)
```

and add it to the `-Werror`/sanitizer target list with
`list(APPEND DZ_OUR_TARGETS op_clip_trim_midi_tests)  # WP-0193c`.

Form: boring assert-based runner like `tests/test_clip_trim.cpp` — no framework, own `main`, a
`CHECK(cond, name)` macro printing `PASS <name>` / `FAIL <name> (file:line)`, exit code = number of
failures, **at least 12 CHECKs**. Scratch files go in
`<std::filesystem::temp_directory_path()>/op_clip_trim_midi_<pid>` (remove a stale one, then create);
never a fixed `/tmp/op_*` path. Read exported positions back with `op::load_midi` (tick / 480 = beats)
rather than writing another SMF parser; the patterns below use distinct MIDI pitches so note pairing
is unambiguous. Use only beat values that are multiples of `0.125` (so every tick is exact) and compare
beats exactly or with `fabs(a - b) <= 1e-9`.

Cover at least:

1. untrimmed clip, two repeats → placement `base + r*E + n.t`, all notes present;
2. `offset_beats` only → material slides to the clip start (`base + (s - w0)`);
3. first exported note is the one at `s = offset_beats` (lower bound inclusive), tick 0;
4. `offset_beats` + `length_beats` → exactly the notes with `w0 <= s < w0 + len`;
5. window end **exclusive**: a clip whose window ends exactly on a note's source position excludes it
   (e.g. four notes at `t = 0..3` with `E = 4`, `offset_beats: 1`, `length_beats: 2` ⇒ `[1, 3)` keeps
   only `s = 1, 2` and drops the note sitting on `s = 3`);
6. non-integer `start_bar` (e.g. `1.5` ⇒ `base = 6` beats) measured after the shift;
7. `length_beats` longer than the material → no extra repeats appear;
8. `offset_beats` past the material (`len <= 0`) → `save_midi` returns `true` and the file has **no**
   note events (`op::load_midi` on it fails with `midi: file contains no playable note events`);
9. durations not shortened: pattern `[{t: 0, dur: 0.5, note: 60}, {t: 3, dur: 2, note: 63}]` (`E = 5`)
   with `offset_beats: 2`, `length_beats: 1.5` ⇒ one exported note at beat `1.0` with
   `dur_beats == 2.0` (it rings past the window end and is not cut);
10. legacy project (no `arrangement`) → every pattern once at bar 0;
11. two clips of the same pattern with different trims → independent windows (`offset_beats` is per clip);
12. exporting the same trimmed project twice gives byte-identical files.

### 4. `docs/spec/project-model.md` — one in-place paragraph, nothing else

At the top of §Clip trim — additive (WP-0193a), replace exactly these two lines:

```
parses, validates, saves and edits the two keys; **rendering and MIDI export ignore them until WP-0193b
(render) and WP-0193c (MIDI) land**. Untrimmed projects are unchanged byte for byte.
```

with exactly these three:

```
parses, validates, saves and edits the two keys; rendering applies them from WP-0193b, MIDI export
(`opd opj2mid`) from WP-0193c. Untrimmed projects are unchanged byte for byte.
```

Do not add a new section (the shared semantics already in §Clip trim cover MIDI export) and do not
touch any other file under `docs/`.

## Acceptance criteria

- [x] Everything above implemented; only `src/engine/midi.cpp`, `tests/test_clip_trim_midi.cpp` (new),
      `CMakeLists.txt` and the one paragraph of `docs/spec/project-model.md` differ from the current HEAD (the integration base: do not switch to main first).
- [x] MIDI export of a trimmed project is exactly `base + (s - offset)` per §1: note-on ticks,
      note-off = on + `dur_beats`, window `[offset, offset+len)` with inclusive lower / exclusive upper
      bound, no extra repeats, nothing exported when `len <= 0`. Verify against the worked example in
      §1 with `opd opj2mid` on a hand-written `.opj` (e.g. `build/opd opj2mid p.opj -o p.mid`).
- [x] Untrimmed exports unchanged: the untrimmed path of §1 is literally the pre-WP code (test item 1 of
      §3 pins the formula), and `harness/run_goldens.sh` reports 0 failures. (Optional cross-check: build
      `main` in a separate gitignored `build-base/` directory and `cmp` both binaries' `opj2mid` output
      over `tests/fixtures/goldens/*.opj` — 0 differences expected.) **Done: 15 files, 0 differences.**
- [x] `tests/test_clip_trim_midi.cpp` with ≥ 12 CHECKs covering items 1–12 of §3; it passes and exits 0.
      **41 CHECKs, ALL PASS, exit 0.**
- [x] `opd opj2mid` exit codes / stdout wording unchanged (`opj->midi: <n> tracks -> <path>`); the input
      project file is not modified.
- [x] Standard gates: build with **0 warnings** (`-j4`, `-Werror` on our targets), `op_tests`,
      `op_clip_trim_midi_tests`, `harness/run_goldens.sh`, `harness/test_cli_errors.sh`,
      `harness/test_glue_scripts.sh`, and a sanitizer build (`-DDZ_SANITIZE=ON`) of
      `op_clip_trim_midi_tests` that runs clean.

## Out of scope

The renderer / choke pre-pass / fades (WP-0193b), the model keys, `set_clip_trim` and `opd diff`
(WP-0193a — already in `main`); `load_midi`, `midi2opj`, `import-midi`; the CLI surface; swing, mute /
solo, master gain and tempo-map export behaviour; `src/app/`, `ai-sidecar/`, harness scripts; any new or
edited golden fixture; `ROADMAP.md` (do not edit it).

## Git

Branch `wp-0193c-clip-trim-midi-export` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0193c: clip trim in MIDI export (opj2mid placement window)`.
Do not push or merge; do not rebase other branches.

## Handoff

### What changed (4 files, all inside the WP's declared scope)

- **`src/engine/midi.cpp`** — only the per-track placement block inside `save_midi`: the local
  `struct Placement` grows the §2 fields (`base_beats`, `src_base`, `w0`, `hi`, `trimmed`) and gets
  default member initializers so the legacy push stays explicit (designated initializers); the clip loop
  builds one proto per clip (`trimmed = (offset_beats != 0.0) || length_beats.has_value()`,
  `len = *length_beats` else `reps*extent - offset_beats`, `hi = w0 + len`) and one placement per repeat
  (`src_base = r*extent`, `offset_beats = base_beats + src_base` — the WP-0129 expression, unchanged);
  the note loop branches on `pl.trimmed`: untrimmed keeps `n.t_beats + pl.offset_beats` verbatim, trimmed
  computes `s = src_base + n.t_beats`, `continue`s outside `[w0, hi)` and places at
  `base_beats + (s - w0)`. Nothing else in the file touched — SMF format/480 ppq, tempo+4/4 meta, name
  meta, velocity bake, `(tick, off-before-on)` sort, delta clamping, legacy no-arrangement path,
  `load_midi`, `import_midi_into`, `midi.hpp` all as they were. Trim names kept separate from the
  WP-0129 `offset_beats` (the naming trap called out in §2).
- **`tests/test_clip_trim_midi.cpp`** — NEW, 326 lines, own `main`, assert-style `CHECK`, exit code =
  failure count. Projects built in memory with the `op::` structs; scratch files under
  `<temp>/op_clip_trim_midi_<pid>` (stale removed, then created); positions read back through
  `op::load_midi` only. All beats multiples of 0.125. Covers §3 items 1–12 plus three extras: 4b window
  spanning a repeat boundary, 13 explicit `offset_beats: 0` byte-identical to an absent key, 14 offset
  landing inside repeat 2 (no leak from repeat 1).
- **`CMakeLists.txt`** — purely additive: the three-line `op_clip_trim_midi_tests` block next to
  `op_clip_trim_tests`, and `list(APPEND DZ_OUR_TARGETS op_clip_trim_midi_tests)  # WP-0193c`. No existing
  line edited or removed.
- **`docs/spec/project-model.md`** — exactly the prescribed two-lines→three-lines replacement at the top of
  §Clip trim; no other file under `docs/` touched.

### How verified (all commands run on this rig: aarch64 DGX Spark, g++ 13.3.0)

Builds went through the `dz-build` skill (shared lock, `-j4`, ccache). Because the sandbox only allows
writes under `/home/shubhankar_dgx/qwen-scratch`, the caches were redirected — `CCACHE_DIR=…/.cache/ccache`
(ccache's default dir is read-only here) and `UV_CACHE_DIR=…/.cache/uv` for the glue suite. No wider access
was requested.

1. **Full `-Werror` build, 0 warnings** (`build-wp0193c`, Release — all targets incl. the new one):
   ```
   $ dz_build.sh "$PWD" --out "$PWD/build-wp0193c"          # -j4, DZ_WERROR=ON on our targets
   /home/shubhankar_dgx/qwen-scratch/openpattern/build-wp0193c
   $ grep -icE "warning|error" build-wp0193c/dz_build.log
   0
   ```
2. **New suite** — 41 CHECKs, all pass, exit 0 (`ctest -R clip_trim` shows it registered):
   ```
   $ ./build-wp0193c/op_clip_trim_midi_tests | tail -3 ; echo $?
   PASS 14 mid-repeat file loads
   PASS 14 window inside repeat 2: nothing from repeat 1 leaks
   ALL PASS
   0
   $ ctest -R "clip_trim" --output-on-failure        # in build-wp0193c
   2/2 Test #11: clip_trim_midi ...................   Passed    0.00 sec
   100% tests passed, 0 tests failed out of 2
   ```
3. **§1 worked example through the CLI** (`out/wp0193c-cli/worked.opj`: notes `t = 0,1,2,3` `dur 0.5`,
   one clip `{start_bar: 0, repeats: 2, offset_beats: 2}`; read back with `opd midi2opj`, tick = t×480):
   ```
   $ build-wp0193c/opd opj2mid worked.opj -o worked.mid ; echo $?
   opj->midi: 1 tracks -> worked.mid
   0
   pitch  62  t_beats 0      tick 0     dur 0.5
   pitch  63  t_beats 1      tick 480   dur 0.5
   pitch  60  t_beats 1.5    tick 720   dur 0.5
   pitch  61  t_beats 2.5    tick 1200  dur 0.5
   pitch  62  t_beats 3.5    tick 1680  dur 0.5
   pitch  63  t_beats 4.5    tick 2160  dur 0.5
   ```
   Exactly the ticks §1 predicts (`0, 480, 720, 1200, 1680, 2160`). **One discrepancy worth a human eye:**
   with the same clip plus `length_beats: 2` the worked example's last sentence says "only `s = 2, 3` play",
   but its own normative rule (and the §2 reference code) window over the *unrolled* timeline, where repeat 1's
   first note sits at `s = 3.5 < w0 + len = 4`. The export therefore gives three notes — beats `0`, `1`, `1.5`
   (pitch 60 at beat 1.5) — which is what the spec §Clip trim semantics and the reference snippet both produce,
   so I implemented the rule rather than the sentence; test 4b pins this reading. If the intended semantics were
   "window only inside repeat 0", §1's rule text, §2's code and `docs/spec` would all need rewording — flagging,
   not changing.
   Bounds and gating are pinned by tests: exclusive end (item 5: note sitting on `s = 3` dropped), inclusive start
   (item 3: first exported note is the one at `s = offset_beats`, tick 0), `len <= 0` (item 8: `save_midi` returns
   true, file written with name meta + end-of-track only, `load_midi` on it fails with
   `midi: file contains no playable note events`), durations untouched (item 9: one note at beat `1.0` with
   `dur_beats == 2.0`, ringing past the window end).
4. **Untrimmed exports byte-identical to the pre-WP binary** — the integration base built in a separate tree
   (`git worktree add …/openpattern-base-wp0193c a94c482` + `dz_build.sh --out …/build-base-wp0193c --target opd`),
   then every golden project exported by both binaries:
   ```
   $ for opj in tests/fixtures/goldens/*.opj; do … base/opd opj2mid … ; new/opd opj2mid … ; cmp -s … ; done
   compared 15 golden .opj files, differences: 0
   ```
   Plus test 1 (formula `base + r*E + n.t`) and test 13 (explicit `offset_beats: 0` ≡ absent key, byte-equal).
5. **Golden renders**: `run_goldens: 15 fixtures, 0 failures` (each rendered twice — bit-exact run to run — then
   diffed against the committed golden, plus the corrupt-golden negative control).
6. **CLI contracts**: `test_cli_errors: 0 failures` (84 PASS / 0 FAIL). `opj2mid` wording and exit code unchanged
   (`opj->midi: 1 tracks -> <path>`, exit 0) and the input project is untouched:
   ```
   $ md5sum worked.opj worked_len.opj > pre.md5   # before the exports above
   $ md5sum -c pre.md5 && echo "input projects unmodified"
   worked.opj: OK
   worked_len.opj: OK
   input projects unmodified
   ```
   Re-exporting the same project is byte-identical (`cmp` clean), matching the determinism promise.
7. **Glue-script e2e**: `test_glue_scripts: 0 failures`. The script hard-codes `$ROOT/build/opd`, so that path was
   symlinked to `build-wp0193c/opd` for the run and the previous `build/opd` binary restored immediately after.
8. **Sanitizers** (`-DDZ_SANITIZE=ON`, Debug, ASAN+UBSAN): `op_clip_trim_midi_tests` builds with 0 warnings and runs
   clean — `ALL PASS`, exit 0, zero sanitizer diagnostics; `op_tests` under the same tree also `ALL PASS (0 failures)`
   with none either. Note the sanitizer tree has to be `CMAKE_BUILD_TYPE=Debug`: at `-O3` GCC 13.3 raises a
   false-positive `-Werror=array-bounds=` in `render.cpp:754` (`out.arrangement = {clip};`) — **pre-existing, not
   from this WP**: an untouched worktree of the integration base fails identically (evidence kept at
   `build-sanbase-wp0193c/dz_build.log`). Every prior sanitizer dir in the repo (`build-san-wp0201`, …) is Debug.

### Metrics

- 4 files touched: `src/engine/midi.cpp` +45/−7, `CMakeLists.txt` +6, spec +3/−2, new test file 326 lines /
  41 CHECKs (requirement ≥ 12). No other repo file modified.
- Suites: new suite 41/41; `op_tests` ALL PASS; goldens 15/15 bit-exact; CLI errors 84/0; glue 0 failures;
  base-vs-new `opj2mid` diff 0 / 15 fixtures.

### Notes & follow-ups

- Nothing outside scope needed changing, so no `PROPOSE:`.
- Pre-existing (out of scope, for whoever owns the renderer): the `-O3` + sanitizer `-Werror=array-bounds`
  false positive in `src/engine/render.cpp:754`. Reproducible on untouched `a94c482`; CI sanitizers job should
  stay Debug (`DZ_BUILD_TYPE=Debug`) as prior WPs did.
- Environment, not repo: this sandbox needs `CCACHE_DIR` and `UV_CACHE_DIR` pointed under
  `/home/shubhankar_dgx/qwen-scratch/.cache/` (default cache dirs are read-only here).
- Scratch kept for the reviewer (all gitignored): `build-wp0193c/`, `build-san-wp0193c/`, plus out-of-tree
  `/home/shubhankar_dgx/qwen-scratch/{openpattern-base-wp0193c,build-base-wp0193c,build-sanbase-wp0193c}`.
  `wpdraft6/` was already untracked in the tree before this WP (another package's draft worktree); left untouched
  and deliberately not committed.

### Git

- Branch `wp-0193c-clip-trim-midi-export` from the integration base `a94c482`; one commit, message
  `WP-0193c: clip trim in MIDI export (opj2mid placement window)` + `Agent: qwen-goldfish`. Not pushed, not merged.

