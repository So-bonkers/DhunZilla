---
id: WP-0181
title: Sampler choke groups — optional track `choke_group`, render-time 5 ms cut of other tracks' voices, set_choke_group op + diff, open/closed hi-hat golden
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.6
human_review: none
touches: [spec:operations, spec:project-model, cmake, model, ops, render, new:tests/test_choke.cpp, new:tests/fixtures/goldens/sampler_choke.opj, new:tests/fixtures/goldens/sampler_choke.wav]
---

## Context — why this exists
Phase 1's exit criterion is "arrange a 3-minute beat with samples". An open hi-hat that keeps ringing over the
closed hi-hat that follows it sounds wrong; every drum machine solves it with *choke groups*. This package adds an
optional integer `choke_group` on **sampler tracks**: a new note on one track cuts the still-sounding voices of every
OTHER track in the same group with a fixed 5 ms linear fade, at render time only. Projects without `choke_group`
render bit-exactly as today (no existing golden may change). One new golden covers open/closed hi-hat.

## Spec — normative
`docs/spec/project-model.md` (new section "Sampler choke groups — additive (WP-0181)") and `docs/spec/operations.md`
(new section "Choke group operation — additive (WP-0181)" plus an amendment to §`opd diff` step 5) are
**already updated by the runner** before you start. Read them first; they are the contract (validation order,
trigger / voice / cut-frame definitions, fade formula, error strings, diff order). Do not edit those two spec files.
Summary (the spec sections win on any difference):

- Track JSON key `choke_group` (track level, NOT inside `voice`): integer `1..16`, optional, sampler tracks only.
  Errors (exact): `track.choke_group must be an integer in [1, 16]` (non-integer incl. `1.0`/`null`/bool/string, or
  out of range; checked right after `solo`, before the voice) and `track.choke_group is only valid for sampler tracks`
  (checked after the voice parsed). Canonical save writes it (after `solo`) only when set.
- Trigger = any note of an **audible** (`track_audible`) track that has a group, with start frame `< total_samples`.
  Start frame = the renderer's placement frame (clips, repeats, tempo map, swing included).
- A voice (note on a grouped sampler track, start frame `s`) is cut at `c` = the smallest trigger start frame
  `c > s` (strictly) of the same group on a **different track**. Same-frame notes never cut each other; notes of the
  same track never cut each other; other groups are ignored.
- Fade: `F = max(1, sample_rate * 5 / 1000)` frames (integer division: 240 @48 kHz, 220 @44.1 kHz). Frame `f` of the
  voice: `f < c` unchanged; `c <= f < c+F` contribution `(sample * track_gain_channel) * g` with
  `g = float(F - d) / float(F)`, `d = f - c`; `f >= c+F` adds nothing.
- Op `set_choke_group {index, group}` (`group` integer or `null` to clear); `opd diff` emits it.

## Implementation guide (4 interface files + CMake + tests + golden)
1. `src/engine/model.hpp` — `struct Track`: add `int choke_group = 0;  // sampler choke group 1..16 (WP-0181); 0 = none` after `solo`.
2. `src/engine/model.cpp` — in the track loop of `project_from_json`, after the `solo` block and before
   `parse_voice`: if `tj.contains("choke_group")`, accept only a JSON integer (`is_number_integer()`; for unsigned
   values compare as `uint64_t`, otherwise as `long long`) in `1..16`, else the first error above; after
   `parse_voice` succeeds, `if (t.choke_group != 0 && t.voice.kind != VoiceKind::Sampler)` → second error. In
   `project_to_json` emit `tj["choke_group"] = t.choke_group` right after the `solo` line, only when `!= 0`.
3. `src/engine/ops.cpp` — `apply_named` branch `set_choke_group` (style of `set_sampler_root`): `check_keys(op_json,
   {"index", "group"})`, `arg_index(... "index" ...)`, `missing argument 'group'`, `'group' must be an integer or
   null` (use `is_number_integer()`; `1.0` is rejected), then `track is not a sampler voice` when
   `j["tracks"][idx]["voice"]["kind"]` is not `"sampler"` (also for `null`), then write `j["tracks"][idx]["choke_group"]`
   (or `erase` it for `null`). Do NOT range-check: the model parse does. In `diff_projects`, inside the per-track
   loop and right after the `set_track_solo` emission, emit
   `{"op": "set_choke_group", "index": i, "group": <b's value, or JSON null when absent>}` when the (absent = null)
   values differ (use `field_or` with a static null json).
4. `src/engine/render.cpp` — in `render_subset`:
   - add `long long choke_at = -1; long long choke_fade = 0;` to `MixTarget`; in `add_stereo`, after the window check,
     when `t.choke_at >= 0 && frame >= t.choke_at`: `d = frame - choke_at`; if `d >= choke_fade` return; else add
     `(l * t.gain_l) * g` / `(r * t.gain_r) * g` with `g = static_cast<float>(choke_fade - d) / static_cast<float>(choke_fade)`.
     With `choke_at < 0` the existing statements run unchanged.
   - Before the track loop, build `std::map<int, std::vector<std::pair<long long, size_t>>> triggers` (group →
     sorted `(start frame, track index)`) from every track with a group for which `track_audible(p, ti)` is true,
     independent of the `subset` argument (so stems see the same cuts as the mix). To enumerate placements without
     duplicating the clip/repeat logic, extract the existing placement code into a small helper used by both the
     pre-pass and the render (e.g. `template <class F> bool for_each_placement(const Project&, size_t track, F&&)`
     calling `f(pattern, offset_beats, clip_gain_db)`) and the existing `t_abs` computation (offset + swing) into a
     helper `note_abs_beat(pat, note, offset)`; the start frame is `tmap.at(note_abs_beat(...))`. This refactor
     must leave every existing golden bit-identical.
   - In the note loop, for a track with `choke_group != 0`: find the earliest trigger frame `c > start` whose
     track index differs (`std::lower_bound` on `(start + 1, 0)`, then skip same-track entries); if found, use a
     copy of the (velocity-adjusted) mix target with `choke_at = c`, `choke_fade = F` for that note's voice. Tracks
     without a group take the existing code path with no extra work.
5. `CMakeLists.txt` — purely ADDITIVE: directly before the comment line starting `# Our targets only` add
   `add_executable(op_choke_tests tests/test_choke.cpp)`, `target_link_libraries(op_choke_tests PRIVATE opengine)`,
   `add_test(NAME choke COMMAND op_choke_tests)`; directly after the line `list(APPEND DZ_OUR_TARGETS op_peaks_tests)  # WP-0171`
   add `list(APPEND DZ_OUR_TARGETS op_choke_tests)  # WP-0181`.
6. `tests/test_choke.cpp` (new; do NOT edit `tests/test_main.cpp`) — its own `main`, a `CHECK(cond, name)` macro that
   prints `PASS name` / `FAIL name` (copy the style of `tests/test_normalize.cpp`), returns the failure count, and
   ends with `ALL PASS` when none failed. Temp files only under `std::filesystem::temp_directory_path()` + a name
   containing `getpid()` (removed at the end). **At least 14 CHECKs**: validation (each bad kind → exact message;
   non-sampler; 1 and 16 accepted; canonical save omits the key by default and round-trips it); render with
   constant-value assets (e.g. 9600 frames of 0.5 and 4800 frames of 0.25, pan -1 so the left channel is the raw value):
   fade is exactly linear and 240 frames long at 48 kHz (frame `c` full level, frame `c+239` is `1/240`, frame
   `c+240` is silent), 220 frames at 44.1 kHz, no cut without a group / with different groups / for notes of the same
   track / for same-frame notes / when the trigger comes after the voice ended, earliest trigger wins, a muted track
   does not trigger, `render_stems` stems show the same cut as the mix; op sets / clears (bytes equal the never-grouped
   bytes) / rejects with the exact messages; `History` undo restores bytes; `diff_projects` emits the op and
   applying it reproduces `b`.
7. **New golden** (authorized: add exactly these two files under `tests/fixtures/`, nothing else, never modify an existing fixture):
   - `tests/fixtures/goldens/sampler_choke.opj` — create it with exactly this JSON content (keys sorted, 2-space
     indent, trailing newline, as the other goldens; it reuses the existing assets `kits/saw48k.wav` = long "open hat"
     and `kits/ramp48k.wav` = short "closed hat", so no new asset file):
     ```json
     {
       "beats_per_bar": 4,
       "bpm": 120.0,
       "format_version": 0,
       "length_bars": 1.0,
       "patterns": [
         {"track": 0, "notes": [{"dur": 1.0, "t": 0.0}, {"dur": 1.0, "t": 1.0}, {"dur": 1.0, "t": 2.0}, {"dur": 1.0, "t": 3.5625}]},
         {"track": 1, "notes": [{"dur": 0.25, "t": 0.125}, {"dur": 0.25, "t": 1.0}, {"dur": 0.25, "t": 3.0}, {"dur": 0.25, "t": 3.5}]}
       ],
       "sample_rate": 48000,
       "seed": 7,
       "tracks": [
         {"choke_group": 1, "gain_db": 0.0, "name": "OpenHat", "pan": 0.0, "voice": {"kind": "sampler", "path": "kits/saw48k.wav"}},
         {"choke_group": 1, "gain_db": 0.0, "name": "ClosedHat", "pan": 0.0, "voice": {"kind": "sampler", "path": "kits/ramp48k.wav"}}
       ]
     }
     ```
     (The layout of the `patterns`/`tracks` lines is free — the check compares the parsed JSON — but the values must
     be exactly these.) What it exercises: open@frame 0 is cut by closed@3000; open and closed on the same frame 24000
     do not cut each other; open@48000 rings to its end; closed@72000 finds nothing to cut; closed@84000 is cut by
     open@85500 (the cut works in both directions).
   - `tests/fixtures/goldens/sampler_choke.wav` — produced by `<build>/opd render sampler_choke.opj -o sampler_choke.wav`
     (96000 frames). `harness/run_goldens.sh` discovers goldens by glob; do not edit it.

## Acceptance criteria
- [x] Defaults bit-exact: all 11 pre-existing goldens pass `bash harness/run_goldens.sh` unchanged; with the new golden the
      script reports 12 fixtures, 0 failures. (Note: this branch's `main` carries **12** pre-existing goldens — WP-0178
      added `clip_fades` after this task was drafted; all 12 pass unchanged, so the script reports 13 with the new one.)
- [x] `opd render` of a project with grouped sampler tracks cuts voices exactly per the spec (fade 5 ms, strict `c > s`,
      other tracks only, audible triggers only, same cuts in `--stems-dir` stems), also with clips/repeats, swing and
      a 44.1 kHz project; a group with a single track renders byte-identically to no group.
- [x] Validation errors (exit 4 from `opd render`) and the op's errors (exit 5 from `opd apply`) use exactly the spec'd strings.
- [x] `opd apply` with `{"op":"set_choke_group","index":1,"group":4}` works; `"group":null` restores the canonical bytes of a
      never-grouped project; undo via `History` is byte-exact.
- [x] `opd diff a.opj b.opj -o j.opjl` for projects differing only in a track's group emits `{"group":4,"index":1,"op":"set_choke_group"}`
      (or `"group":null`), and applying the journal reproduces b.
- [x] `build/op_choke_tests` (≥ 14 CHECKs) ends `ALL PASS`; `build/op_tests` too.
- [x] Standard gates: build with 0 warnings (`-Werror` default, `-j4` max), `bash harness/test_cli_errors.sh`,
      `bash harness/test_glue_scripts.sh`, sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_choke_tests`.
- [x] ADR-0004 respected: no clock/randomness, no libm calls (only `+ - * /`, integer arithmetic, `std::lower_bound`/`std::sort`).

## Out of scope — do NOT touch
- `ROADMAP.md` (the elephant adds the row at ship time), existing golden fixtures (read-only; only the two new files above),
  `src/app/`, `src/cli/`, `src/doc/`, `ai-sidecar/`, `.github/`, `harness/`, `docs/adr/`, `docs/spec/*` (already updated),
  `tests/test_main.cpp`.
- Choke for synth/sine/metronome voices, per-note / per-velocity choke, configurable fade length, choke across a mute
  boundary, `opd lint` / `opd steps` awareness, an app UI for groups, MIDI import of groups.

## Git
- Branch `wp-0181-sampler-choke` from `main`. Commit: `WP-0181: sampler choke groups (choke_group, 5 ms cut) + set_choke_group op + diff + golden`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` — `Track::choke_group` (int, default 0 = none) added after `solo`.
  - `src/engine/model.cpp` — parse: key accepted only as JSON integer 1..16 (unsigned/signed compare per spec), checked
    right after `solo`, before the voice; sampler-only check after `parse_voice`; both exact error strings. Canonical save
    emits `choke_group` only when != 0 (inserted after `solo`; keys dump alphabetically as everywhere in this format).
  - `src/engine/ops.cpp` — `set_choke_group {index, group}` branch (style of `set_sampler_root`, no second ruleset: range
    left to the model parse) + `diff_projects` step-5 emission after `set_track_solo` via `field_or` with a static null.
  - `src/engine/render.cpp` — `MixTarget::choke_at/choke_fade`; `add_stereo` drops frames ≥ cut+F and scales the fade
    window by `g = float(F-d)/float(F)` (multiplication order `(sample * gain_channel) * g`; with `choke_at < 0` the
    pre-existing statements run unchanged). Placement code extracted verbatim into `for_each_placement()` +
    `note_abs_beat()` shared by the render loop and the new trigger pre-pass (group → sorted `(start frame, track)` over
    all audible grouped tracks, independent of `subset`, so stems see the same cuts as the mix); per-note cut via
    `choke_cut_frame()` (`std::lower_bound` on `(start+1, 0)`, skipping same-track entries). `F = max(1, sr*5/1000)`
    integer division. No libm/clock/RNG added (ADR-0004).
  - `CMakeLists.txt` — purely additive: `op_choke_tests` target + ctest registration before the `# Our targets only`
    comment, and `list(APPEND DZ_OUR_TARGETS op_choke_tests)  # WP-0181` after the WP-0171 line.
  - `tests/test_choke.cpp` (new) — 28 CHECKs: validation (bad kinds incl. `1.0`/null/bool → exact msg; check order vs
    solo and voice; non-sampler; 1 and 16 accepted), canonical save (omitted by default, written when set, byte round-trip),
    render (fade exactly linear 240 frames @48k incl. frame c full / c+239 = 1/240 / c+240 silent; 220 frames @44.1k;
    no cut without group, with other groups, same-track, same-frame, trigger after voice end; earliest trigger wins;
    muted track does not trigger; single-track group byte-identical to no group; stems == mix; clips+repeats+swing
    placement → cut at frame 101100), op (set/clear bytes-equal/6 exact rejections incl. untouched project on failure;
    non-sampler also for null), `History` undo byte-exact, diff emits `{"group":4,...}` / `{"group":null,...}` and the
    journal reproduces the target bytes.
  - New goldens (only these two fixture files added): `tests/fixtures/goldens/sampler_choke.opj` (exact JSON from the task)
    + `sampler_choke.wav` rendered by `build/opd render`.
  - `docs/spec/operations.md`, `docs/spec/project-model.md` — included exactly as installed by the runner, unmodified.
- **How verified:** (all on this aarch64 rig; `UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv` for uv)
  - Build `-Werror -j4`: clean, zero warnings (`cmake --build build -j4` → `[100%] Built target op_tests`).
  - `./build/op_choke_tests` (28 CHECKs): all PASS then `ALL PASS`, exit 0. `./build/op_tests`: 956 PASS lines,
    `ALL PASS (0 failures)`, exit 0.
  - Pre-existing goldens with the modified renderer (before adding the new fixture):
    `bash harness/run_goldens.sh` → 12× `PASS … deterministic + golden match` + negative control,
    `run_goldens: 12 fixtures, 0 failures`. With the new golden: `run_goldens: 13 fixtures, 0 failures`, exit 0.
  - Golden WAV content checked against the spec formula in Python (left channel, pan-0 equal-power ×cos(π/4), ±3 LSB):
    fade@3000 `(saw*g + ramp[d])*PAN` for all 240 frames ✓; silent-from-3240 = `ramp` only ✓; frames [0,3000) and the
    same-frame-24000 span [24000,33600) and open@48000 span [48000,57600) byte-identical to an ungrouped render ✓;
    reverse cut: fade@85500 `(saw[d] + ramp[1500+d]*g)*PAN` ✓, closed silent after 85740 ✓ → `GOLDEN CONTENT VERIFIED`.
  - CLI contracts (scratch dir with `kits/` symlink):
    ```
    opd render bad_str.opj  → error: parse: track.choke_group must be an integer in [1, 16]        exit=4
    opd render bad_sine.opj → error: parse: track.choke_group is only valid for sampler tracks     exit=4
    opd diff a.opj b.opj -o j.opjl → {"group":4,"index":1,"op":"set_choke_group"}
      opd apply a.opj j.opjl -o … && cmp … b.opj                                                  PASS applied(a) == b byte-exact
    opd diff b.opj a.opj -o jr.opjl → {"group":null,"index":1,"op":"set_choke_group"}
      opd apply b.opj jr.opjl -o … && cmp … a.opj                                                  PASS null-clear applied(b) == a byte-exact
    opd apply j17 (group 17) → error: apply: line 1: set_choke_group: track.choke_group must be an integer in [1, 16]   exit=5
    opd apply jd  (group 1.5) → error: apply: line 1: set_choke_group: 'group' must be an integer or null               exit=5
    ```
  - Stems via CLI (`opd render sampler_choke.opj --stems-dir`): `0_openhat.wav` shows the fade (±3 LSB), is exactly
    silent over [3240, 24000) while its next note at frame 24000 still sounds; mix L during the fade == stem L + closed
    contribution → `STEMS MATCH MIX`.
  - `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`; `bash harness/test_glue_scripts.sh` →
    `test_glue_scripts: 0 failures`.
  - Sanitizers: `cmake -B build-san -DDZ_SANITIZE=ON && cmake --build build-san -j4 --target op_tests op_choke_tests` →
    both built; `./build-san/op_choke_tests` → `ALL PASS`; `./build-san/op_tests` → `ALL PASS (0 failures)`; no ASAN/UBSAN reports.
  - Metrics: engine diff +~120 lines across 4 files; test file 28 CHECKs; golden render 96000 frames @48 kHz.
- **PROPOSE / BLOCKED:**
  - No blockers. Observation (no action taken): the task text says "11 pre-existing goldens / 12 fixtures", but `main`
    already has 12 (`clip_fades` from WP-0178 landed after drafting) — evidence above reflects the real counts (12 → 13).
  - `PROPOSE:` the spec sentence "Canonical save writes `choke_group` (after `solo`, before `voice`)" describes insertion
    intent; actual canonical dumps sort keys alphabetically (nlohmann default), so `choke_group` lands first in the track
    object — same behaviour every other track key already has. Worth a one-word spec clarification next time these files
    are touched (spec is out of scope here, so untouched).
