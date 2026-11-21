---
id: WP-0191
title: Project time signature — optional `time_signature {num, den}` + `set_time_signature` op + `opd diff` + `opd info` line + 3/4 and 6/8 goldens
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [model, ops, cli:main, tests:main, "new:tests/fixtures/goldens/time_sig_3_4.opj", "new:tests/fixtures/goldens/time_sig_3_4.wav", "new:tests/fixtures/goldens/time_sig_6_8.opj", "new:tests/fixtures/goldens/time_sig_6_8.wav"]
---

## Context — why this exists
Every DhunZilla bar is `beats_per_bar` quarter-note beats (default 4). Musicians think in time signatures: 3/4,
6/8, 5/4. This package adds an optional project key `time_signature {num, den}` whose only job is to **derive
`beats_per_bar`**. All bar→beat mapping in the engine already goes through `beats_per_bar` (render clip
placement, tempo-map bars, `opd render --from-bar/--to-bar`, `opd click` downbeat accents), so once the model
parse derives it, those all honour the signature with **no change to `render.cpp`**. Do not edit `render.cpp`.
Legacy projects (no `time_signature` key) must parse, save and render exactly as before.

## Spec — normative (this task file is the contract; `docs/spec/*` are NOT edited by this package)

### Project JSON key
`time_signature`: object with exactly the two keys `num` and `den`.
- `num`: JSON **integer** (a float such as `3.0`, a string, a bool, null are all rejected) in `[1, 32]`.
- `den`: JSON integer, one of `2`, `4`, `8`, `16`.
- Bar length in quarter-note beats is `num * 4 / den` and must be a whole number: `(num * 4) % den == 0`.
  Examples: 3/4 → 3, 6/8 → 3, 2/2 → 4, 12/8 → 6, 5/4 → 5; 7/8 and 9/8 are rejected.
- Bar length must be `<= 32` beats (so `32/2` = 64 is rejected).
- The parse **derives** `Project::beats_per_bar = num * 4 / den`. If the file also has a `beats_per_bar` key it
  must equal the derived value, else error. If the file has no `beats_per_bar`, the derived value is used (note:
  the legacy default 4 is only used when *neither* key is present).
- Normalisation: when `den == 4` the parsed project does **not** keep a `time_signature` (the signature is fully
  carried by `beats_per_bar == num`); for `den != 4` it is kept.

Parse error strings (exact; `opd render`/`opd info` print them as `error: parse: <string>`, exit 4). Checks run in
this order, first failure wins, and all run after the existing `beats_per_bar` > 0 check:
1. not an object, or not exactly the keys `num` and `den`: `time_signature must be an object {num, den}`
2. bad `num`: `time_signature.num must be an integer in [1, 32]`
3. bad `den`: `time_signature.den must be 2, 4, 8 or 16`
4. not whole: `time_signature bar must be a whole number of quarter-note beats`
5. too long: `time_signature bar must be at most 32 quarter-note beats`
6. `beats_per_bar` key present and different from derived: `beats_per_bar conflicts with time_signature`

### Canonical save
`project_to_json` writes the `time_signature` key (`{"num": N, "den": D}`) **only** when the project holds one
(den != 4). `beats_per_bar` is always written as today. A legacy project's saved bytes are unchanged.

### `TimeSignature` and `effective_time_signature`
Both live in `namespace op` (all of `src/engine/model.hpp` is in `namespace op`): `struct TimeSignature { int num = 4; int den = 4; };`
and
`inline TimeSignature effective_time_signature(const Project& p)` in `src/engine/model.hpp`: the stored
`time_signature` if present, else `{p.beats_per_bar, 4}`.

### Op `set_time_signature`
`{"op":"set_time_signature","num":<int>,"den":<int>}`. Exactly keys `op`, `num`, `den` (unknown key →
`unknown argument 'x'` as for every op). `num` and `den` must be JSON integers (helper `has_integer`; messages
`missing argument 'num'`, `'num' must be an integer`, likewise `den`). The op sets `j["time_signature"]` to
`{num, den}` in the working JSON copy and **erases** `j["beats_per_bar"]`; the existing save→parse validation then
derives `beats_per_bar` and produces every range/derivation error above. Do NOT duplicate the range rules in the
op. Errors surface as `set_time_signature: <message>` (apply exit 5, nothing written). The op is undoable by the
existing `History` (no new undo code). Setting `{4,4}` on any project yields no `time_signature` key and
`beats_per_bar` 4; setting `{3,4}` yields no key and `beats_per_bar` 3.
Note: bar-anchored fields (`start_bar`, `length_bars`, tempo-map `bar`, `loop`) keep their bar values; their beat
positions change with the bar length. This is intended.

### `opd diff`
- `beats_per_bar` is **no longer an "unsupported difference"**: remove that guard. (The existing fixed order of
  the other guards is unchanged; a seed difference is still reported first.)
- Effective signature of a project = its `time_signature` object if present else `{"num": beats_per_bar, "den": 4}`.
  When the effective signatures of a and b differ, emit exactly one op `set_time_signature` with b's effective
  `num`/`den`, placed **immediately before** the `set_bpm` op position (i.e. right before the
  `if (ja.at("bpm") != jb.at("bpm"))` emission). Emitted line text: `{"den":8,"num":6,"op":"set_time_signature"}`
  (the journal writer sorts keys). Applying the journal to a reproduces b's bytes (4/4→3/4, 3/4→6/8, 6/8→4/4 all
  round-trip).

### `opd info`
For a project file, after the `project: …` line and before the `master_gain_db=` line, print
`time_signature: N/D\n` using `effective_time_signature`, **only when it is not 4/4**. A project whose effective signature is 4/4
(including a legacy project with `beats_per_bar` 4) is byte-identical to before. A legacy project with
`beats_per_bar` ≠ 4 gets the line, since its effective signature is not 4/4 (e.g. `beats_per_bar` 3 → `time_signature: 3/4`).

### What the other commands do (no code change, must keep working — the check probes them)
- `opd render` of a project with `time_signature` {6,8} is **bit-identical** to the same project written with
  `"beats_per_bar": 3` and no `time_signature`. Same for {3,4}.
- `opd render --from-bar 1 --to-bar 2` of a 3/4 or 6/8 project renders exactly 72000 frames at 120 bpm / 48 kHz
  (bar = 3 beats); a 4/4 project renders 96000.
- `opd click` of a 6/8 project is bit-identical to `opd click` of the `beats_per_bar: 3` equivalent (downbeat
  accent every 3 quarter-note beats). Click beats are always quarter notes, even in /8 and /16 signatures.

## Implementation guide (5 code files + 4 golden fixtures, ~125 changed code lines)
1. `src/engine/model.hpp` — `struct TimeSignature { int num = 4; int den = 4; };` placed before `struct Project`;
   `std::optional<TimeSignature> time_signature;` as the **last** member of `Project`; the inline
   `effective_time_signature` after the struct.
2. `src/engine/model.cpp` — in `project_from_json`, right after the `beats_per_bar <= 0` check and the
   `length_bars` check (i.e. before `master_gain_db` is read): `p.time_signature.reset();` then the validation
   above. Use `is_number_integer()` for `num`/`den`; read values as `double` for the range test before casting to
   `int` so huge numbers cannot overflow. In `project_to_json`, immediately after
   `j["beats_per_bar"] = p.beats_per_bar;` add the key only when `p.time_signature` is set.
3. `src/engine/ops.cpp` — `apply_named`: new branch next to `set_loop` (style of `set_loop`: `check_keys`,
   `has_integer`; `j["time_signature"] = json{{"num",…},{"den",…}}; j.erase("beats_per_bar");`). `diff_projects`:
   replace the `beats_per_bar` guard and add the emission as specified.
4. `src/cli/main.cpp` — `cmd_info` line only (2 lines). `#include`s already suffice.
5. `tests/test_main.cpp` — new `static void test_time_signature()` called from `main` (after `test_player()`),
   **≥ 15 new `CHECK(` lines**, using the existing `tmp_file()` helper (unique per-pid temp dir; never fixed
   `/tmp` paths): parse of 6/8 (derived 3, kept) and 3/4 (derived 3, key dropped), legacy unchanged bytes, every
   error string above, the conflict rule, op set/undo/redo/reject/missing-arg/float-arg, diff emit +
   reproduce + back-to-4/4.
6. Goldens (new, read-only afterwards): `tests/fixtures/goldens/time_sig_3_4.opj` and `time_sig_6_8.opj`, each with
   a `time_signature` key (`{"num": 3, "den": 4}` and `{"num": 6, "den": 8}` respectively — the key must be literally present in the `.opj`), a metronome track with `accent_every: 3`, a synth lead pattern placed by an
   `arrangement` clip at `start_bar: 1.0`, `length_bars` 3 and 2 respectively, 120 bpm, 48 kHz. Render each with
   the built `opd render <file>.opj -o <file>.wav` and commit the `.wav` beside it. `bash harness/run_goldens.sh`
   must report them `deterministic + golden match`. Do not modify any existing golden.

## Acceptance criteria
- [x] Legacy projects: every pre-existing golden unchanged; `opd apply` with an empty journal on a legacy
      project writes bytes with **no** `time_signature` key.
- [x] A project with `"time_signature":{"num":6,"den":8}` loads, has `beats_per_bar` 3, saves with
      `"time_signature"` and `"beats_per_bar": 3`; `{"num":3,"den":4}` loads with `beats_per_bar` 3 and saves with
      no `time_signature` key.
- [x] Every parse error string above is produced exactly (`error: parse: …`, exit 4 from `opd render`).
- [x] `opd apply` of `set_time_signature` works, is undoable via `History`, rejects bad input with
      `set_time_signature: <message>` (exit 5, no output file written).
- [x] `opd diff` emits/round-trips `set_time_signature` as specified; `opd info` prints `time_signature: 6/8`
      for a 6/8 project and nothing extra for 4/4.
- [x] Bar→beat mapping: render / `--from-bar --to-bar` / click probes in "What the other commands do" hold.
- [x] Standard gates: build (`-Werror`, 0 warnings, `-j4` max), `build/op_tests` ends `ALL PASS`,
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh`
      green, sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` passes.
- [x] ADR-0004: no clock, no randomness, no new libm use, no new dependency.

## Out of scope — do NOT touch
- `ROADMAP.md`, `docs/` (spec files are folded in by the elephant at ship time), existing golden fixtures,
  `src/engine/render.cpp`, `src/engine/midi.cpp` (MIDI export of /8 signatures is deferred), `src/app/`,
  `src/doc/`, `ai-sidecar/`, `opd new` (`--beats-per-bar` unchanged), `CMakeLists.txt`.
- Odd /8 bars (7/8, 9/8), fractional bar lengths, mid-song signature changes, click beats on the /8 pulse,
  `time_signature` inside `tempo_map`.

## Git
- Branch `wp-0191-time-signature-project-model` from `main`. Commit: `WP-0191: project time_signature + set_time_signature op + diff + info + 3/4 & 6/8 goldens`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include this task file with the handoff filled.
- **Do not push or merge.**
- The `docs/spec/` update for this feature is deferred on purpose: the elephant folds it in at ship time (this overrides AGENTS.md's definition-of-done for docs for this package only).

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp`: `struct TimeSignature { int num = 4; int den = 4; };` before `Project`;
    `std::optional<TimeSignature> time_signature;` as last `Project` member; inline
    `effective_time_signature(const Project&)` after the struct.
  - `src/engine/model.cpp`: in `project_from_json`, right after the `beats_per_bar`/`length_bars` checks and
    before `master_gain_db`: key-shape → `num` → `den` → whole-beats → ≤32 → conflict validation with the six
    exact error strings, deriving `p.beats_per_bar = num * 4 / den`, normalising away `den == 4`; range tests
    read `get<double>()` before casting (no overflow). In `project_to_json`, `time_signature` written only when
    held (immediately after `beats_per_bar`).
  - `src/engine/ops.cpp`: new `set_time_signature` branch next to `set_loop` (`check_keys{"num","den"}` +
    `has_integer`; sets `j["time_signature"]`, erases `j["beats_per_bar"]`; all range rules come from the
    save→parse, none duplicated). `diff_projects`: removed the `beats_per_bar` unsupported guard; effective
    signature compared and on difference exactly one `set_time_signature` with b's values emitted immediately
    before the `set_bpm` emission.
  - `src/cli/main.cpp`: `cmd_info` prints `time_signature: N/D` via `effective_time_signature`, only when ≠ 4/4,
    after the `project:` line and before `master_gain_db=` (2 lines).
  - `tests/test_main.cpp`: new `test_time_signature()` called from `main` right after `test_player()`;
    **39 CHECKs** covering parse derive/keep (6/8) and normalise (3/4), conflict rule, all six parse error
    strings (incl. float/string/bool/huge num), legacy save-bytes stability + round-trip,
    `effective_time_signature`, op set/undo/redo via `History`, `{3,4}`/`{4,4}` normalisation, reject /
    missing-arg / float-arg / unknown-key with exact `set_time_signature: …` strings and untouched project,
    diff emit line text, ordering before `set_bpm`, and byte-level reproductions 4/4→6/8, 3/4→6/8, 6/8→4/4.
  - New golden fixtures (read-only afterwards; no existing golden touched): `time_sig_3_4.opj` /
    `time_sig_6_8.opj` with literal `time_signature` keys, metronome `accent_every: 3`, synth lead pattern via
    arrangement clip at `start_bar: 1.0`, `length_bars` 3 / 2, 120 bpm, 48 kHz; `.wav`s rendered with the built
    `opd render` and committed beside them.
  - `src/engine/ops.hpp`: doc-comment-only update of `diff_projects` (the header listed `beats_per_bar` as an
    unsupported diff field; that is no longer true). No interface change — flagged here because ops.hpp is not
    in the package's `touches` list.
- **How verified:** (all commands run on this aarch64 rig; branch `wp-0191-time-signature-project-model`)
  - Build, `-Werror`, fresh configure, `-j4` — 0 warning/error lines:
    `cmake -S . -B build-wp0191 && cmake --build build-wp0191 -j4 | grep -ciE "warning|error"` → `0`
  - Unit tests: `./build-wp0191/op_tests` → `ALL PASS (0 failures)`; `op_tests | grep -c "^PASS ts\."` → `39`.
  - Goldens (`bash harness/run_goldens.sh`, also re-run against the fresh build):
    ```
    PASS time_sig_3_4: deterministic + golden match
    PASS time_sig_6_8: deterministic + golden match
    PASS negative-control (corrupt golden rejected)
    run_goldens: 15 fixtures, 0 failures
    ```
    (all 13 pre-existing goldens still pass → legacy renders unchanged.)
  - CLI exit-code contracts: `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  - Glue e2e (`UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv bash harness/test_glue_scripts.sh`)
    → `test_glue_scripts: 0 failures`. (First run failed with `Failed to spawn: opsidecar` — a stale
    ai-sidecar `.venv` missing the console-script entry point; fixed by environment-only
    `uv sync --reinstall-package dhunzilla-sidecar`, no repo changes.)
  - Sanitizers: `cmake -S . -B build-san-wp0191 -DDZ_SANITIZE=ON && cmake --build … -j4` (0 warning/error lines)
    then `./build-san-wp0191/op_tests` → `ALL PASS (0 failures)` under ASAN+UBSAN.
  - Sidecar CI-parity (untouched by this WP): `uv run pytest -q` → `146 passed, 2 skipped`;
    `uv run ruff check .` → `All checks passed!`.
  - Legacy: every pre-existing golden unchanged (above). Empty-journal apply on a legacy canonical project →
    ```
    apply: 0 op(s) -> …/out.opj
    IDENTICAL          # cmp -s vs input bytes
    0 occurrences      # grep -c '"time_signature"' out.opj
    ```
  - Load/save round-trip: `{"num":6,"den":8}` → `beats_per_bar` 3, saved with both keys (grep of saved file):
    `"time_signature": { "den": 8, "num": 6 }`, `"beats_per_bar": 3`; `{"num":3,"den":4}` → `beats_per_bar` 3,
    saved without the key (`ts.parse_3_4_derives_3_drops_key`, `ts.save_3_4_no_time_signature_key`).
  - Parse errors via `opd render` (all exit 4):
    ```
    error: parse: time_signature must be an object {num, den}            exit=4
    error: parse: time_signature.num must be an integer in [1, 32]       exit=4
    error: parse: time_signature.den must be 2, 4, 8 or 16               exit=4
    error: parse: time_signature bar must be a whole number of quarter-note beats   exit=4
    error: parse: time_signature bar must be at most 32 quarter-note beats          exit=4
    error: parse: beats_per_bar conflicts with time_signature            exit=4
    ```
  - `opd apply set_time_signature` reject (7/8): `error: apply: line 1: set_time_signature: time_signature bar
    must be a whole number of quarter-note beats`, `exit=5`, output file not created (`ls` → No such file).
    Undo/redo through `op::History`: `ts.op_undo_restores_4_4`, `ts.op_redo_back_to_6_8`.
  - `opd diff` line text + round-trip (unit-verified byte-level; CLI probe):
    ```
    $ opd diff a.opj b.opj -o d.opjl && cat d.opjl
    {"den":8,"num":6,"op":"set_time_signature"}
    ```
    Ordering before `set_bpm`: journal == `{"den":8,…}\n{"bpm":90.0,"op":"set_bpm"}` (`ts.diff_emitted_before_set_bpm`).
  - `opd info`: legacy 4/4 prints no extra line (output byte-shape unchanged); golden prints
    `time_signature: 6/8` after the `project:` line; 3/4 golden prints `time_signature: 3/4`.
  - Bar→beat mapping probes (no render.cpp change): 6/8 and 3/4 goldens render **bit-identical** (`cmp`) to
    hand-written `"beats_per_bar": 3` equivalents; `opd render --from-bar 1 --to-bar 2`:
    ```
    rendered 72000 frames @48000 Hz -> r34.wav (seed=34)   # time_sig_3_4.opj
    rendered 72000 frames @48000 Hz -> r68.wav (seed=68)   # time_sig_6_8.opj
    rendered 96000 frames @48000 Hz -> r44.wav (seed=42)   # demo.opj (4/4)
    ```
    `opd click` of the 6/8 golden bit-identical (`cmp`) to the `beats_per_bar: 3` equivalent.
  - ADR-0004 scan of added lines (`git diff | grep "^+" | grep clock|random|rand(|libm|#include`): only false
    positives on "expect"/"explicit" — no clock, no randomness, no new libm use, no new dependency.
- **Metrics:** 276 insertions / 3 deletions across 6 code files (model.hpp/cpp, ops.cpp/hpp, cli/main.cpp,
  test_main.cpp) + 4 new golden fixtures; 39 new CHECKs.
- **PROPOSE / BLOCKED:** none blocked. PROPOSE: `docs/spec/project-model.md` and `operations.md` need the
  time-signature section folded in at ship time (deferred by this package per its Git section); MIDI export of
  /8 signatures remains deferred (out of scope here).

