---
id: WP-0193a
title: Clip trim, part A — optional clip keys `offset_beats` / `length_beats` (parse, validate, canonical save) + undoable `set_clip_trim` op + `opd diff` support
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [spec:project-model, spec:operations, model, ops, cmake, "new:tests/test_clip_trim.cpp"]
---

## Context — why this exists
A DAW clip can be trimmed: start playing the pattern from some beat inside it (`offset_beats`) and/or stop after a
number of beats (`length_beats`). WP-0193 is split into three parts: **A (this one)** adds the data model, the edit
operation and `opd diff`; **B** applies the window in the renderer; **C** applies it in MIDI export. In part A the
two keys are stored and edited but **render and MIDI export completely ignore them** (a trimmed project renders
exactly like the untrimmed one). Projects without the keys must behave and save byte-for-byte as before.

## Spec — normative
`docs/spec/project-model.md` (section "Clip trim — additive (WP-0193a)") and `docs/spec/operations.md` (sections
"Clip trim operation" and the "`opd diff` and clip trim" amendment) already contain this text — the runner copies
them in before you start; do not edit `docs/`. This task file is the contract; the summary below is complete.

### Clip keys (both optional, in each `arrangement` clip object)
- `offset_beats`: JSON number (integer or float form), finite, in `[0, 4096]`, default `0`.
- `length_beats`: JSON number, finite, in `(0, 4096]`; absent = untrimmed.
- Values are read as `double`. Integers beyond 32 bits (`4294967296`, `18446744073709551615`) and huge floats
  (`1e300`) are just out of range — never cast to `int`, never wrapped. `-0.0` is a valid offset (equals 0).
- Anything that is not a JSON number (string, bool, `null`, array, object) is rejected.

Exact parse errors (the whole `err` string; `opd` prints `error: parse: <string>`, exit 4). Checked in
`project_from_json` for each clip **right after the existing `clip.fade_out_beats` check**, offset first, then
length, first failure wins:
- `clip.offset_beats must be a number in [0, 4096]`
- `clip.length_beats must be a number in (0, 4096]`

### Data model (`src/engine/model.hpp`, struct `Clip`)
Append two members after `fade_out_beats`, exactly:
```cpp
    double offset_beats = 0.0;                          // source start inside the unrolled pattern (WP-0193a)
    std::optional<double> length_beats = std::nullopt;  // trimmed length; absent = untrimmed (WP-0193a)
```
Keep the `= std::nullopt` initializer: `tests/test_main.cpp` aggregate-initialises `Clip` with fewer values and
`-Werror=missing-field-initializers` rejects a member without a default initializer. Do not edit `tests/test_main.cpp`.

### Canonical save (`project_to_json`)
In the clip object write `offset_beats` only when it is `!= 0.0`, and `length_beats` only when present (both as
`double`). Key order is the canonical writer's own sorted order. A clip with neither key serialises exactly as before.

### Op `set_clip_trim`
`{"op":"set_clip_trim","index":I,"offset_beats":X,"length_beats":Y}`. Exactly these four keys. Implement as a new
branch in `apply_named` (`src/engine/ops.cpp`) right before the `remove_clip` branch, in the style of
`set_clip_fades`. Validation order and exact messages (all surface as `set_clip_trim: <message>`):
1. unknown key → `unknown argument 'x'` (existing `check_keys`);
2. `index` via the existing `arg_index` / clip-count idiom (→ `'index' out of range`);
3. for `offset_beats` then `length_beats`: key absent → `missing argument 'offset_beats'` / `missing argument 'length_beats'`;
   value neither number nor `null` → `'offset_beats' must be a number or null` / `'length_beats' must be a number or null`.
`null` **clears** that side: erase the key from the working clip JSON (offset → 0, length → untrimmed); a number is
stored as given. **Do not add range checks to the op**: the existing save→parse validation after the branch
rejects bad values with the two parse errors above, so e.g. `offset_beats: 5000` fails as
`set_clip_trim: clip.offset_beats must be a number in [0, 4096]`. A rejected op leaves the project unchanged.
The op is undoable through the existing `History` (no new undo code). `{"offset_beats":0, ...}` is accepted and
saves without the key. Via `opd apply` a rejected op exits 5 as for every op, e.g.
`error: apply: line 1: set_clip_trim: 'length_beats' must be a number or null`.
`add_clip` needs no change: it carries the clip JSON, so trim keys inside it are validated by the same parse.

### `opd diff` (`diff_projects` in `src/engine/ops.cpp`)
Inside the per-shared-clip loop, **immediately after the `set_clip_fades` emission**, compare the canonical-JSON
keys `offset_beats` and `length_beats` of clip i of a and b (an absent key compares as `null`); if either differs
emit ONE op:
`{"index":i,"length_beats":<b value or null>,"offset_beats":<b value or null>,"op":"set_clip_trim"}`
(as written by the journal writer, keys sorted; `null` when b's clip lacks the key). Examples (a untrimmed, b with
offset 1.0 and length 2.0): `{"index":0,"length_beats":2.0,"offset_beats":1.0,"op":"set_clip_trim"}`; (b → a, i.e. clearing both):
`{"index":0,"length_beats":null,"offset_beats":null,"op":"set_clip_trim"}`. Nothing is emitted when both sides
agree, so existing journals are byte-identical. Applying the journal to a reproduces b's bytes. Existing ops keep their existing position (e.g. `set_clip_gain` stays before the fades op, so for one clip the order is gain, fades, trim). Clips added by
`add_clip` already carry their trim keys; do not emit trim ops for them.

## Existing interfaces you will call (already in the repo, do not change)
- `#include "engine/ops.hpp"`: `bool op::apply_op(op::Project&, const nlohmann::json& op, std::string& err)`;
  `op::History` with `bool apply(op::Project&, const nlohmann::json&, std::string& err)`, `bool undo(op::Project&)`,
  `bool redo(op::Project&)`.
- `#include "engine/model.hpp"`: `std::string op::project_bytes(const op::Project&)` (canonical bytes).
- `#include "engine/model_json.hpp"`: `op::project_from_json`.

## Implementation guide (exactly 5 files: `touches:` entries starting with `spec:` are runner-supplied and are NOT counted; ~50 code lines + the test file)
1. `src/engine/model.hpp` — the two members above (`<optional>` is already included).
2. `src/engine/model.cpp` — parse (after the fade_out_beats block, same style: `is_number()`, `std::isfinite`,
   `get<double>()`) and save as specified.
3. `src/engine/ops.cpp` — op branch + diff emission.
4. `tests/test_clip_trim.cpp` (NEW, own `main`, own `CHECK(cond, name)` macro printing `PASS name` / `FAIL name  (file:line)`,
   returns the failure count, prints `ALL PASS` when none; do NOT edit `tests/test_main.cpp`). It must not write
   files; build projects with `nlohmann::json` + `op::project_from_json` (`engine/model_json.hpp`) in memory.
   **At least 25 `CHECK(` calls (each call on a line that starts with `CHECK(` after indentation)** covering: defaults, accepted extremes (`0`, `4096`, `0.001`, `-0.0`, integer form),
   canonical save (keys omitted at default, written when set, byte-identical re-parse), every parse error string for
   both keys (negative, above max, `0` length, string, `null`, `4294967296`, `18446744073709551615`), error order
   (offset before length; fades before trim), op set/null-clears-each-side/null-both-restores-bytes, op errors
   (unknown key, missing key, non-number, range via parse, index out of range, rejected = bytes unchanged),
   `History` undo/redo byte-exact, diff text for set / clear / partial-clear, diff replay reproduces b, no op when equal,
   fades op before trim op, `add_clip` carrying trim keys.
5. `CMakeLists.txt` — purely ADDITIVE (change no existing line): immediately after the line
   `add_test(NAME meters COMMAND op_meters_tests)` add
   `add_executable(op_clip_trim_tests tests/test_clip_trim.cpp)`,
   `target_link_libraries(op_clip_trim_tests PRIVATE opengine)`,
   `add_test(NAME clip_trim COMMAND op_clip_trim_tests)`; and immediately after the line
   `list(APPEND DZ_OUR_TARGETS op_meters_tests)  # WP-0194` add `list(APPEND DZ_OUR_TARGETS op_clip_trim_tests)  # WP-0193a`
   (so `-Werror` and the sanitizer flags reach it).

Note: `project_from_json` appends to the project's `arrangement`; parse into a fresh `op::Project` each time.

## Acceptance criteria
- [ ] Clips without the keys: every golden unchanged, `opd apply` with an empty journal writes no `offset_beats` /
      `length_beats` key.
- [ ] Both keys parse, validate (exact strings above, `error: parse: …`, exit 4 from `opd apply`/`opd info`), save canonically and round-trip.
- [ ] `set_clip_trim` as specified (null clears, errors, rejection leaves bytes unchanged, undo/redo byte-exact, exit 5 via `opd apply`, no output file written on failure).
- [ ] `opd diff` emits / round-trips `set_clip_trim` exactly as specified.
- [ ] `opd render` and `opd opj2mid` output of a trimmed project is **identical** to that of the same project without the two keys (the keys are ignored in this part).
- [ ] `op_clip_trim_tests` ends `ALL PASS` with >= 25 CHECKs.
- [ ] Standard gates: build (`-Werror`, 0 warnings, `-j4` max), `build/op_tests` ends `ALL PASS`,
      `bash harness/run_goldens.sh`, `bash harness/test_cli_errors.sh`, `bash harness/test_glue_scripts.sh` green,
      sanitizer build (`-DDZ_SANITIZE=ON`) of `op_tests` and `op_clip_trim_tests` passes.
- [ ] ADR-0004: no clock, no randomness, no new libm use (`std::isfinite` is fine), no new dependency.

## Out of scope — do NOT touch
- `ROADMAP.md`, `docs/` (spec text is supplied), golden fixtures (`tests/fixtures/`), `src/engine/render.cpp`,
  `src/engine/midi.cpp` (parts B and C), `tests/test_main.cpp`, `src/cli/`, `src/app/`, `src/doc/`, `.github/`,
  `ai-sidecar/`, `docs/adr/`.
- Applying the trim window anywhere (render, MIDI, fades at the trimmed end), clip split, a trim UI, `opd info` output,
  `duplicate_clip` changes (it already copies the whole clip JSON).

## Git
- Branch `wp-0193a-clip-trim-model-op` from `main`. Commit: `WP-0193a: clip trim keys + set_clip_trim op + opd diff`.
  Stage explicit paths only (never `git add -A` / `git add .`). Include `TASKS/WP-0193a-clip-trim-model-op.md` (this task file) with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` — appended to `struct Clip`, exactly as specified: `double offset_beats = 0.0;` and
    `std::optional<double> length_beats = std::nullopt;` after `fade_out_beats`. No other member touched.
  - `src/engine/model.cpp` — parse: two checks added right after the `fade_out_beats` block (offset first, then
    length; `is_number()` + `std::isfinite` + range, `get<double>()`, exact spec error strings). Save:
    `offset_beats` written only when `!= 0.0`, `length_beats` only when present — a clip with neither key
    serialises as before.
  - `src/engine/ops.cpp` — new `set_clip_trim` branch in `apply_named` immediately before `remove_clip`, in the
    `set_clip_fades` style (`check_keys` → `arg_index` → per-key missing / "number or null"; `null` erases the key
    from the working clip JSON; no range checks — save→parse validates). `diff_projects`: one `set_clip_trim`
    emitted right after the `set_clip_fades` emission when either canonical key differs (absent compares as
    `nullptr`; op carries b's value or `null`).
  - `tests/test_clip_trim.cpp` — NEW, own `main`, 72 `CHECK(` calls, in-memory only (no file writes).
  - `CMakeLists.txt` — purely additive: `op_clip_trim_tests` target + `add_test(NAME clip_trim …)` after the
    meters lines, and `list(APPEND DZ_OUR_TARGETS op_clip_trim_tests)  # WP-0193a`.
  - `docs/spec/project-model.md`, `docs/spec/operations.md` — runner-installed spec text, committed unchanged.
- **How verified:** (all commands run on this aarch64 rig; branch `wp-0193a-clip-trim-model-op`)
  - **Build `-Werror`, `-j4`:** `cmake --build build -j4` → exit 0, `grep -ci warning` over the full build output = `0`.
  - **Unit tests:** `./build/op_clip_trim_tests` → ends `ALL PASS`, exit 0 (72 CHECKs ≥ 25). Head/tail:
    ```
    PASS base project parses
    ...
    PASS diff adds the trimmed clip via add_clip, no separate trim op
    ALL PASS
    trim_exit=0
    ```
    `./build/op_tests` → `ALL PASS (0 failures)`.
  - **Goldens:** `bash harness/run_goldens.sh` → `run_goldens: 13 fixtures, 0 failures` (all deterministic +
    bit-exact golden match; clips without the keys unchanged).
  - **CLI contracts:** `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
    `UV_CACHE_DIR=… bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - **Sanitizers:** `cmake -S . -B build-san-wp0193a -DDZ_SANITIZE=ON && cmake --build … -j4 --target op_tests
    op_clip_trim_tests` (0 warnings) → `./op_tests` = `ALL PASS (0 failures)`, `./op_clip_trim_tests` = `ALL PASS`.
  - **Empty journal writes no keys:** `opd apply base_c.opj empty.opjl -o applied_empty.opj` → exit 0,
    `grep -c 'offset_beats\|length_beats'` on the output = `0`; canonical form idempotent (re-apply byte-identical).
  - **Parse errors, exact strings, exit 4:**
    ```
    info bad_off_str: 'error: parse: clip.offset_beats must be a number in [0, 4096]' exit=4
    info bad_len_zero: 'error: parse: clip.length_beats must be a number in (0, 4096]' exit=4
    apply bad_off_neg: 'error: parse: clip.offset_beats must be a number in [0, 4096]' exit=4   (no output file written)
    ```
  - **`set_clip_trim` via `opd apply`:** set → exit 0, output carries `"length_beats": 2.0` / `"offset_beats": 1.0`;
    rejected ops → exit 5 with no output file:
    ```
    C2: 'error: apply: line 1: set_clip_trim: clip.offset_beats must be a number in [0, 4096]' exit=5
    C3: 'error: apply: line 1: set_clip_trim: 'length_beats' must be a number or null' exit=5
    ```
    Undo/redo byte-exact and all op/parse error strings covered by the unit test (see CHECK names above).
  - **`opd diff`:** journal texts match the spec examples verbatim and replay reproduces the target bytes:
    ```
    {"index":0,"length_beats":2.0,"offset_beats":1.0,"op":"set_clip_trim"}   (a→b, cmp byte-identical after apply)
    {"index":0,"length_beats":null,"offset_beats":null,"op":"set_clip_trim"} (b→a, cmp byte-identical after apply)
    ```
    Equal projects → `diff: 0 op(s)`, journal file 0 bytes. Gain/fades/trim order and add_clip-carries-keys in the unit test.
  - **Render/MIDI ignore the keys:** `opd render base_c.opj -o a.wav --seed 42` vs trimmed → `cmp`: bit-identical
    (1536044 B each); `opd opj2mid` both → byte-identical (99 B each).
  - **ADR-0004:** no clock, no randomness, no new libm beyond `std::isfinite`, no new dependency.
- **Metrics:** ~50 code lines across the 3 existing files + 270-line test file; 72 CHECKs.
- **PROPOSE / BLOCKED:** nothing blocked; no out-of-scope changes needed. Note for WP-0193b/c: the semantics
  paragraph in `docs/spec/project-model.md` (window `[offset, offset+len)`, onset gating only, fades end at
  `base + len`) is untouched here — render and MIDI keep ignoring both keys, as required for part A.
