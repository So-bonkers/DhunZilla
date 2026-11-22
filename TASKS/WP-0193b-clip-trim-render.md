---
id: WP-0193b
title: Clip trim in render — apply the offset/length window in for_each_placement, the choke pre-pass and render_pattern_at; fades end at base + len
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [render, cmake, spec:project-model, "new:tests/test_clip_trim_render.cpp", "new:tests/fixtures/goldens/clip_trim.opj", "new:tests/fixtures/goldens/clip_trim.wav"]
---

## Context — why this exists

WP-0193a (already in `main`) added the two optional clip keys `offset_beats` / `length_beats`, the
`set_clip_trim` operation and its `opd diff` emission. **The renderer still ignores both keys**: a trimmed
clip plays its whole unrolled pattern. This package makes rendering honour the window, so a clip behaves
the way an editor user expects: the source material slides under the clip's start, and its audible end is
where the trim says it is.

Three places in `src/engine/render.cpp` enumerate notes and they must agree by construction, not by
copy-paste — they all go through one helper:

- `for_each_placement(p, ti, tmap, f)` — the single placement enumerator (spec §arrangement): legacy path
  (no arrangement) plus one callback call per clip repeat.
- the **choke trigger pre-pass** inside `render_subset` (WP-0181) — it enumerates every note of every
  audible track that has a `choke_group` through `for_each_placement`, so windowed-out notes must stop
  being triggers automatically.
- `render_pattern_at` — the placement lambda that renders one placement's notes (called through
  `for_each_placement`).

Naming warning (this is a real trap in this file): `render.cpp` already has a local name `offset_beats`
that means **placement beat** (`note_abs_beat(pat, note, offset_beats)` and the second callback argument of
`for_each_placement`). It is *not* the clip's trim offset. Do not introduce a second meaning for it: keep
clip-trim values under new names (`w0`, `len`, `trimmed`, `ClipGate`) and leave every existing use of
`offset_beats` (including `note_abs_beat`'s parameter and the lambda parameters) exactly as it is: **no renames**.

Read first, do not change their contracts: `pattern_extent()` and `Clip` in `src/engine/model.hpp`;
`note_abs_beat()`, `ClipEnv`, `clip_env_gain()`, `MixTarget`, `TempoMap` and `choke_cut_frame()` in
`src/engine/render.cpp`. `docs/spec/project-model.md` §Arrangement (clips), §Explicit pattern length,
§Pattern swing, §Sampler choke groups and §Clip trim (WP-0193a) are the current behaviour.

## Spec

### 1. The window (normative — identical to §Clip trim in `docs/spec/project-model.md`)

Per clip: `E = pattern_extent(pat)`; effective repeats `R` = `clip.repeats` when `E > 0`, else `1`
(the existing rule); `base = clip.start_bar * beats_per_bar`; `w0 = clip.offset_beats`; and
`len = *clip.length_beats` when the key is present, else `R*E - w0`. The clip's *source* timeline is the
unrolled pattern: note `n` of repeat `r` sits at source position `s = r*E + n.t`.

- A note **plays** iff `w0 <= s < w0 + len`; it is placed at absolute beat `base + (s - w0)`. Swing still
  keys off the pattern-local `n.t` (§Pattern swing), never off the shifted position.
- Note durations are **not** shortened: only onsets are gated. A surviving note sounds for its full
  `dur_beats`; use `fade_out_beats` to cut sound.
- `len <= 0` ⇒ the clip contributes nothing at all — no notes and no fade envelope.
- A `length_beats` longer than the remaining material leaves silence: repeats are never re-generated to
  fill it.
- The clip end used by the WP-0178 fades is **`base + len`**: `fade_in_beats` runs from `base`,
  `fade_out_beats` ends at `base + len`.
- Only notes that play are choke triggers: a trimmed-out note neither sounds nor cuts another track.
- An **untrimmed** clip (`offset_beats` absent or exactly `0`, `length_beats` absent) keeps the
  pre-WP-0193b path bit for bit: `w0 = 0`, `len = R*E`, no gating, placement `base + r*E`, clip end
  `base + R*E`. All committed goldens must stay bit-identical.
- A trim never changes the project's render length (`length_bars` / `total_samples_for` are untouched):
  audio simply stops earlier inside the same buffer.

### 2. `src/engine/render.cpp` — exact shape of the change

Add next to `note_abs_beat()` (which is unchanged):

```cpp
struct ClipGate {
    bool gated = false;
    double lo = 0.0;  // inclusive lower bound on note.t_beats
    double hi = 0.0;  // exclusive upper bound on note.t_beats
};

inline bool note_plays(const ClipGate& g, const Note& n) {
    return !g.gated || (n.t_beats >= g.lo && n.t_beats < g.hi);
}
```

`for_each_placement` — the callback gains a fifth argument, `const ClipGate& gate` (its second argument
keeps its name and now carries `base + (src - w0)`; update the doc comment to mention the new argument):

- legacy path (`p.arrangement.empty()`) passes `ClipGate{}` (never gates).
- per clip, before the repeat loop:
  ```cpp
  const bool trimmed = clip.offset_beats != 0.0 || clip.length_beats.has_value();
  const double w0 = clip.offset_beats;
  const double len =
      clip.length_beats ? *clip.length_beats : static_cast<double>(reps) * extent - w0;
  if (trimmed && !(len > 0.0)) continue;   // empty window: the clip plays nothing at all
  ClipGate gate;
  gate.gated = trimmed;
  ```
  (`reps` is the existing effective-repeat variable; untrimmed ⇒ `w0 == 0`, so every expression below
  reduces to the legacy one exactly.)
- envelope: `clip_end_beats` becomes `base + len` (identical to today's `base + reps*extent` when
  untrimmed). Everything else about the envelope stays as it is.
- per repeat `r`: `const double src = static_cast<double>(r) * extent;`, then when `trimmed` set
  `gate.lo = w0 - src; gate.hi = w0 + len - src;` and call
  `f(pat, base + (src - w0), clip.gain_db, env, gate)`.

Both consumers of the enumerator gate identically — first statement inside their note loops:

- choke trigger pre-pass in `render_subset`: `if (!note_plays(gate, note)) continue;`
- `render_pattern_at` (now taking `const ClipGate& gate` as its fifth parameter, forwarded by the
  `for_each_placement` call): same skip.

No other file in `src/` changes. Do not touch `midi.cpp`, `model.*`, `ops.cpp`, `src/cli/main.cpp`, or any
header: this package is renderer-internal plus tests, spec and one golden.

### 3. New test file and CMake registration

Create `tests/test_clip_trim_render.cpp` — own `main()`, boring `CHECK(cond, "name")` macro printing
`PASS <name>` / `FAIL <name>`, exit code = number of failures (same style as `tests/test_choke.cpp`). It
builds projects in memory with `nlohmann::json` + `op::project_from_json`, renders with
`op::render_project(p, base_dir, buf, err)` and compares whole interleaved buffers **bit-exactly**
(`std::memcmp`) — no tolerances. Any file it writes goes into a unique scratch directory
(`std::filesystem::temp_directory_path()` + a name containing `::getpid()`) and is removed before exit;
never a fixed `/tmp/op_*` path. Keep it compact (about 150 lines; the whole package has a 250-line cap).

At least 12 `CHECK`s covering, with the exact behaviours of §1:
untrimmed identity (explicit `"offset_beats": 0.0`; `"length_beats"` equal to the material); a whole-repeat
window equals one untrimmed repeat; a partial window spanning repeats equals the flattened equivalent
pattern (pins `base + (s - offset)`); a window starting in a later repeat of a clip at bar 1; `len <= 0`
renders all zeros; a length past the material adds no audio; swing survives the shift (equals the untrimmed
render shifted by exactly `offset` frames); fades end at `base + len` (equals an untrimmed pattern whose
extent is `len`, with and without an offset); and the choke pre-pass respects the window — a long note on
one sampler track and, on another, a clip whose window excludes its first note: the excluded note must not
cut the long note (it plays on past the beat where the excluded note would have triggered and is cut only
by the surviving placement), while the untrimmed variant of that clip does cut early.

Register it in `CMakeLists.txt`, immediately after the `op_clip_trim_tests` block, with exactly:

```cmake
add_executable(op_clip_trim_render_tests tests/test_clip_trim_render.cpp)
target_link_libraries(op_clip_trim_render_tests PRIVATE opengine)
add_test(NAME clip_trim_render COMMAND op_clip_trim_render_tests)
```

and add `list(APPEND DZ_OUR_TARGETS op_clip_trim_render_tests)  # WP-0193b` next to the other appends.

### 4. New golden `clip_trim`

Create `tests/fixtures/goldens/clip_trim.opj` with **exactly** this content (it exercises a window that
cuts across repeats, swung notes, a fade-out ending at `base + len`, and a second trimmed clip on another
track), then generate the golden from it:

```bash
./build/opd render tests/fixtures/goldens/clip_trim.opj -o tests/fixtures/goldens/clip_trim.wav
```

```json
{
  "arrangement": [
    {
      "fade_out_beats": 1.0,
      "length_beats": 6.0,
      "offset_beats": 3.0,
      "pattern": 0,
      "repeats": 4,
      "start_bar": 0.0
    },
    {
      "gain_db": -3.0,
      "offset_beats": 1.0,
      "pattern": 1,
      "repeats": 3,
      "start_bar": 2.0
    }
  ],
  "beats_per_bar": 4,
  "bpm": 120.0,
  "format_version": 0,
  "length_bars": 4.0,
  "patterns": [
    {
      "notes": [
        {
          "dur": 1.0,
          "t": 0.0
        },
        {
          "dur": 1.0,
          "t": 0.75
        },
        {
          "dur": 1.0,
          "t": 2.0
        },
        {
          "dur": 1.0,
          "t": 3.0
        }
      ],
      "swing": 0.25,
      "track": 0
    },
    {
      "notes": [
        {
          "dur": 0.5,
          "t": 0.0
        },
        {
          "dur": 0.5,
          "t": 1.5
        }
      ],
      "track": 1
    }
  ],
  "sample_rate": 48000,
  "seed": 2027,
  "tracks": [
    {
      "gain_db": -2.0,
      "name": "Lead",
      "pan": 0.3,
      "voice": {
        "freq_hz": 437.0,
        "kind": "sine"
      }
    },
    {
      "name": "Bass",
      "voice": {
        "freq_hz": 110.0,
        "kind": "sine"
      }
    }
  ]
}
```

`harness/run_goldens.sh` auto-discovers `goldens/*.opj`, so no other registration is needed; the golden is
checked for bit-exact determinism (render twice, compare) plus match against your generated `.wav`.

### 5. Spec document

Append a `## Clip trim — render (WP-0193b)` section to `docs/spec/project-model.md` documenting §1 (the
runner supplies the full replacement file; keep it as delivered).
Total change budget: <= 250 changed lines excluding the golden pair.

## Acceptance criteria

- [ ] §2 implemented in `src/engine/render.cpp`; no other `src/` file changes.
- [ ] `tests/test_clip_trim_render.cpp` exists, is registered in `CMakeLists.txt` exactly as in §3, and
      passes with ≥ 12 `CHECK`s covering every bullet of §1 (including the choke pre-pass).
- [ ] Untrimmed projects are bit-identical: all 15 pre-existing goldens still match
      (`bash harness/run_goldens.sh`).
- [ ] New golden pair `tests/fixtures/goldens/clip_trim.opj` + `clip_trim.wav` present, the `.opj` exactly
      as in §4, and it renders deterministically and matches its `.wav` (16 fixtures, 0 failures).
- [ ] Trim behaviour observable through the CLI: rendering `clip_trim.opj` with `--format f32` has audio
      only where §1 predicts it (Lead ends at beat `5.78125 + 1`, Bass plays beats 8.5, 9, 10.5, 11 and
      12.5 only, nothing after beat 13).
- [ ] Standard gates: build with **zero warnings** (`-Wall -Wextra -Werror`, `-j4`), `op_tests`,
      `op_clip_trim_render_tests`, `harness/run_goldens.sh`, `harness/test_cli_errors.sh`,
      `harness/test_glue_scripts.sh`, and a sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` and
      `op_clip_trim_render_tests` passing.
- [ ] `docs/spec/project-model.md` carries the new section (§5).

## Out of scope

- **MIDI export** (`src/engine/midi.cpp`, `opd opj2mid`): keeps ignoring the window — that is WP-0193c.
  Do not touch `midi.cpp`.
- Model/ops/CLI: the keys, `set_clip_trim` and `opd diff` already exist (WP-0193a). No new CLI flags, no
  changes to `src/engine/model.*`, `src/engine/ops.*`, `src/cli/main.cpp` or any header.
- No change to project length semantics (`length_bars`, `total_samples_for`), tempo maps, mixer, limiter,
  normalize, stems behaviour, or the fade envelope math itself (WP-0178) beyond which beat is the clip end.
- Do not edit `ROADMAP.md`; do not modify any existing file under `tests/fixtures/` (including
  `goldens/`) except adding the two new `clip_trim.*` files; no new dependencies, no fast-math, no
  wall-clock reads (ADR-0004).

## Git

Branch `wp-0193b-clip-trim-render` from the current HEAD (the integration base: do not switch to main first). Commit: `WP-0193b: apply clip trim window in render`.
Do not push or merge.

## Handoff report

### What changed (249 changed lines, budget 250, golden pair excluded)

| file | ins | del | note |
| --- | ---: | ---: | --- |
| `src/engine/render.cpp` | 43 | 11 | §2 exactly: `ClipGate` + `note_plays()` next to `note_abs_beat()`; fifth callback arg `const ClipGate& gate` on `for_each_placement`; window (`w0`/`len`/`trimmed`) + `continue` on empty window; `clip_end_beats = base + len`; per-repeat `src`, `gate.lo/hi`, placement `base + (src - w0)`; gate skip as first statement in both note loops (choke pre-pass and `render_pattern_at`). No renames — every existing `offset_beats` use untouched. |
| `CMakeLists.txt` | 4 | 0 | the three-line block from §3 verbatim, immediately after `op_clip_trim_tests`, plus the `DZ_OUR_TARGETS` append next to the others |
| `tests/test_clip_trim_render.cpp` | 181 | 0 | new, own `main()`, 14 `CHECK`s, bit-exact `memcmp`, scratch dir `temp_directory_path()/dz_clip_trim_render_<pid>` removed at exit |
| `docs/spec/project-model.md` | 10 | 0 | §5 section as delivered by the runner (kept verbatim) |
| `tests/fixtures/goldens/clip_trim.opj` + `.wav` | — | — | new golden pair (§4); `.opj` byte-identical to this file's fenced JSON, verified below |

No other `src/` file touched: `git diff --stat src/` → `src/engine/render.cpp | 54 ++++...` only. `midi.cpp`,
`model.*`, `ops.*`, `src/cli/main.cpp` and all headers unchanged.

### Acceptance criteria — evidence

Commands run in `/home/shubhankar_dgx/qwen-scratch/openpattern` on branch `wp-0193b-clip-trim-render`
(base HEAD `a94c482`). Builds via the `dz-build` skill script (shared lock, `-j4`, gcc 13.3.0); build dirs
`/home/shubhankar_dgx/qwen-scratch/.builds/wp-0193b` (Release) and `.../wp-0193b-asan` (`Debug -DDZ_SANITIZE=ON`).
`DZ_NO_CCACHE=1` was needed: this sandbox's ccache dir is read-only (`ccache: error: Read-only file system`).

**Zero warnings.** `grep -ci warning .builds/wp-0193b/dz_build.log` → `0`; the project sets `-Wall -Wextra`
and `-Werror` on `DZ_OUR_TARGETS`, so a clean build is the proof. Same for the sanitizer build (`BUILD=0`).

**New test binary — 14 CHECKs, exit 0:**

```
$ .builds/wp-0193b/op_clip_trim_render_tests ; echo $?
PASS untrimmed.explicit_offset_zero_identity
PASS untrimmed.length_equals_material_identity
PASS window.one_whole_repeat_equals_single
PASS window.spanning_repeats_equals_flattened
PASS window.starts_in_later_repeat_at_bar1
PASS length.past_material_adds_nothing
PASS empty.len_zero_all_silence
PASS empty.len_negative_all_silence
PASS swing.equals_untrimmed_shifted_by_offset_frames
PASS fade.ends_at_base_plus_len
PASS fade.with_offset_ends_at_base_plus_len
PASS choke.untrimmed_variant_cuts_early
PASS choke.excluded_note_does_not_cut
PASS choke.surviving_trigger_cuts
0
```

§1 coverage: untrimmed identity (explicit `"offset_beats": 0.0`; `length_beats` == material), whole-repeat
window == one untrimmed repeat, partial window across repeats == flattened equivalent (pins `base + (s - w0)`),
window starting in a later repeat of a bar-1 clip, `len == 0` and `len < 0` → all zeros, length past material
adds nothing, swing shifted by exactly 24000 frames (`eq_shift`, zero tail), fades ending at `base + len` with
and without offset, choke pre-pass (excluded note does not cut; surviving trigger cuts at its **shifted** frame
36000 = `(2.0 − 0.5) × 24000`; untrimmed variant cuts at 3000).

**Pre-existing goldens bit-identical + new golden (16 fixtures, 0 failures):**

```
$ bash harness/run_goldens.sh /home/shubhankar_dgx/qwen-scratch/.builds/wp-0193b/opd
PASS arranged: deterministic + golden match
PASS clip_fades: deterministic + golden match
PASS clip_trim: deterministic + golden match
PASS demo: deterministic + golden match
PASS imported: deterministic + golden match
PASS mixer: deterministic + golden match
PASS panorama: deterministic + golden match
PASS pitched: deterministic + golden match
PASS resampled: deterministic + golden match
PASS sampler_choke: deterministic + golden match
PASS sampler_repitch: deterministic + golden match
PASS sampler_trim: deterministic + golden match
PASS synth_v1: deterministic + golden match
PASS tempo: deterministic + golden match
PASS time_sig_3_4: deterministic + golden match
PASS time_sig_6_8: deterministic + golden match
PASS negative-control (corrupt golden rejected)
run_goldens: 16 fixtures, 0 failures
```

All 15 pre-existing `.wav` fixtures matched unchanged — the untrimmed path is bit-for-bit intact. The new
`.opj` is byte-identical to §4 (`diff` of the fenced block against the file → no output, `OPJ MATCHES SPEC VERBATIM`).

**CLI observability (`--format f32`, raw buffer analysed frame-exactly):**

```
$ build/opd render tests/fixtures/goldens/clip_trim.opj -o out.wav --format f32
rendered 384000 frames @48000 Hz -> out.wav (seed=2027)
PASS lead_gap_2.78125_to_3_silent
PASS lead_audio_to_6.78125
PASS gap_lead_end_to_bass_start_silent
PASS bass_8.5_to_9.5_audible
PASS bass_gap_9.5_to_10.5_silent
PASS bass_10.5_to_11.5_audible
PASS bass_gap_11.5_to_12.5_silent
PASS bass_12.5_to_13_audible
PASS nothing_after_beat_13
PASS frame_312000_exact_zero
```

Lead's last onset is beat `5.78125` (window `[3,9)` keeps repeat 2's `t = 0.75`, swung) and its audio ends at
`5.78125 + 1`; Bass plays beats 8.5, 9, 10.5, 11, 12.5 only; exact silence from beat 13 (frame 312000) to end.

**Standard gates:**

```
$ .builds/wp-0193b/op_tests | tail -1        → ALL PASS (0 failures)          exit 0
$ bash harness/test_cli_errors.sh <opd>      → test_cli_errors: 0 failures    exit 0
$ ln -sf <opd> build/opd; bash harness/test_glue_scripts.sh   (UV_CACHE_DIR set per sandbox)
                                             → test_glue_scripts: 0 failures  exit 0
$ ctest -N | grep clip_trim                  → Test #10: clip_trim, Test #11: clip_trim_render
```

**Sanitizers (`Debug`, `-DDZ_SANITIZE=ON` = ASAN+UBSAN, `-fno-sanitize-recover=undefined`):**

```
$ .builds/wp-0193b-asan/op_tests | tail -1              → ALL PASS (0 failures)   exit 0
$ .builds/wp-0193b-asan/op_clip_trim_render_tests | tail -1 → PASS choke.surviving_trigger_cuts   exit 0
```

### Notes / follow-ups

- **`Release + -DDZ_SANITIZE=ON` fails on this machine, before and after this package.** gcc 13.3 rejects the
  *pre-existing* `out.arrangement = {clip};` in `pattern_mode_project` with
  `-Werror=array-bounds=` ("forming offset 72 is out of the bounds [0, 72] of object '<anonymous>'").
  Reproduced on untouched base HEAD `a94c482` (`.builds/wp-0193b-asanbase/dz_build.log`, same single error),
  so it is not caused by WP-0193b and was not worked around here. The CI sanitizer job configures with
  `-DCMAKE_BUILD_TYPE=Debug -DDZ_SANITIZE=ON` (`.github/workflows/ci.yml`) and that combination builds clean —
  the Debug sanitizer run above is the one that matches CI. `PROPOSE:` pin `CMAKE_BUILD_TYPE=Debug` for the
  sanitizer build in the dz-build skill so nobody re-discovers this.
- The choke CHECKs initially failed against my own arithmetic, not the renderer: a trimmed-out trigger note
  also shifts its surviving siblings, so the cut lands at `(2.0 − offset) × spb`, not at `t × spb`. Encoding
  that in the expectation was worth doing — it is exactly the "agree by construction" property §1 asks for.
- `wpdraft6/` (a nested, unrelated WP-0199 checkout that was already present as untracked before this package
  started) was left untouched and uncommitted; it is outside this branch's scope.

