---
id: WP-0179
title: Pattern ids + `duplicate_pattern {src, new_id}` — deep-copy a pattern under a new unique name, undoable, appears in `opd diff` as a plain pattern add
phase: 1
status: ready
depends_on: []
estimate_agent_hours: 0.5
human_review: none
touches: [model, ops, tests:main, spec:operations, spec:project-model]
---

## Context — why this exists

Patterns are index-addressed only today: `{"op":"add_note","pattern":1}` and `clip.pattern` speak array
positions, so a copied idea ("try another version of the kick groove") has no name anywhere in the project,
and nothing can be duplicated in one operation. This package adds the smallest honest version of both:

1. an **optional unique name** on a pattern — the JSON key `id` (`std::string id;` on `op::Pattern`), and
2. one operation **`duplicate_pattern {src, new_id}`** that appends a deep copy of `patterns[src]` whose `id`
   is `new_id`, rejecting an unknown source index and a name that is already taken.

It is a pure model/operation change: no CLI subcommand, no renderer change, no new file format. Ids never
enter the audio path — **every existing golden stays bit-identical**, and a project without ids saves
byte-for-byte as before.

## Spec — normative

`docs/spec/project-model.md` (new section **"Pattern ids — additive (WP-0179)"**) and `docs/spec/operations.md`
(new section **"Pattern duplicate operation — additive (WP-0179)"**, plus amendments to §`opd diff`) are
**already updated by the runner** before you start. Read those two sections first; they are the contract
(value rules, validation order and strings, canonical-save position, append semantics, diff behaviour). Do not
edit those two spec files. The summary below repeats every string this package's tests assert — if it ever
disagrees with the spec sections, the spec sections win.

### Model (`docs/spec/project-model.md` §Pattern ids)

* Pattern JSON gains an optional key `id`: a **JSON string**, **non-empty**, at most **64 bytes**
  (`std::string::size()`), **unique across the patterns of one project** that declare it (exact byte compare,
  case-sensitive). Absent = unnamed. It is identity only: renderer, MIDI import/export, `opd lint`, `opd steps`
  and the mixer never read it.
* Model field: `std::string id;` on `struct Pattern`, default `""` = unnamed. Indices stay the addressing
  scheme everywhere — nothing that takes a pattern index changes.
* Parse validation (`load_project`, same code path operations use), checked **right after `pattern.track` and
  before `swing`**, first failure wins, each line is the whole error string:
  1. present but not a JSON string (number, `null`, boolean, object, array) → `pattern.id must be a string`
  2. empty string → `pattern.id must not be empty`
  3. more than 64 bytes → `pattern.id must be at most 64 bytes`
  4. equal to the id of an **earlier** pattern in the same file → `pattern.id 'X' is already in use`
     (`X` = the id verbatim inside single quotes; patterns are scanned in array order, so the later one fails)
* **Canonical save:** `"id"` is written only when non-empty. Canonical JSON dumps object keys alphabetically,
  so in a pattern object `id` lands first: `id`, `length_beats`, `notes`, `swing`, `track`. No key-order code is
  needed — just insert the assignment anywhere in the pattern save loop.

### Operation (`docs/spec/operations.md` §Pattern duplicate operation)

```jsonc
{"op": "duplicate_pattern", "src": <int>, "new_id": <string>}
```

Appends to `patterns` a **deep copy** of `patterns[src]` with `"id"` replaced by `new_id`. `<n>` = number of
patterns. Apply semantics unchanged (copy → JSON edit → save→parse validation → commit; no second ruleset).
Argument checks, in exactly this order, error strings `duplicate_pattern: <reason>`:

1. unknown key → `unknown argument '<key>'` (only `op`, `src`, `new_id` allowed);
2. `src` an integer in `[0, n)` — reuse the existing `arg_index()` helper verbatim, so the messages are exactly
   `missing argument 'src'`, `'src' must be an integer` (also for `0.0` and `"0"`), `'src' out of range`
   (negative or `>= n`; this is the "unknown source pattern" case);
3. `new_id` a required JSON string — reuse `has_string()`: `missing argument 'new_id'`,
   `'new_id' must be a string` (numbers, `null`, booleans, objects, arrays all get that one message);
4. the edit appends the copy with `"id": new_id`; then **the model parse enforces the id rules** — do NOT
   re-implement them in the op: empty → `duplicate_pattern: pattern.id must not be empty`, over 64 bytes →
   `duplicate_pattern: pattern.id must be at most 64 bytes`, already used by any other pattern (including the
   source's own id) → `duplicate_pattern: pattern.id 'X' is already in use`.

Effect details (all normative):

* **Deep copy:** every field of the source pattern object is copied — `track`, `swing`, `length_beats` when
  present, and all notes with their `t`, `dur`, `note` and `vel` keys, in order. Only `"id"` changes. A source
  without `swing`/`length_beats` yields a copy without them.
* **The copy is appended**: it becomes `patterns[n]`. No existing pattern index shifts, no clip is added, moved
  or re-pointed, and the source is untouched — unlike `duplicate_clip`, which inserts at `index+1`. Because of
  that, duplicating never changes a single output sample of an arrangement that does not reference the copy.
* **Undoable** with the existing `History` whole-project snapshots: undo restores the exact original bytes
  (including the absence of the extra pattern); redo re-applies them byte-exactly. A rejected apply leaves the
  project and both stacks untouched.
* `add_pattern` may also carry `"id"` in its pattern JSON — no extra code is needed; a duplicate id there is
  refused by the model parse (`add_pattern: pattern.id 'X' is already in use`).

### `opd diff` (`docs/spec/operations.md` §`opd diff` and pattern ids)

* `duplicate_pattern` is **never emitted** by `opd diff`. When `b` is `a` plus a duplicated pattern, the journal
  is exactly one step-8 `add_pattern` whose `pattern` JSON is b's canonical copy (including its `id`), and
  applying that journal to `a` reproduces `save_project(b)` byte-exactly. The reverse direction stays a plain
  tail `remove_pattern`.
* An id difference on a **shared** pattern index cannot be expressed (no rename op exists), so it becomes an
  unsupported difference: `unsupported difference: patterns[<i>].id`, checked **immediately after** the existing
  `patterns[i].track` check for the same ascending `i` (first hit reports; absent counts as "unnamed", so
  present-vs-absent on a shared pattern differs too). Projects whose patterns all have equal ids produce exactly
  the same journal bytes as before this WP.

## Implementation guide (4 interface files + tests)

1. `src/engine/model.hpp` — in `struct Pattern`, immediately after the line
   `int track = 0;               // index into Project::tracks` add
   `std::string id;` with a short comment (`optional name (WP-0179): "" = unnamed; non-empty ids unique`).
2. `src/engine/model.cpp` — in the pattern loop of `project_from_json`, immediately after the closing brace of
   the `pattern.track out of range:` check and before the `// Pattern swing (WP-0166 …` comment, add exactly the
   four rules above, in that order:

   ```cpp
   if (pj.contains("id")) {
       const json& iv = pj.at("id");
       if (!iv.is_string()) { err = "pattern.id must be a string"; return false; }
       pat.id = iv.get<std::string>();
       if (pat.id.empty()) { err = "pattern.id must not be empty"; return false; }
       if (pat.id.size() > 64) { err = "pattern.id must be at most 64 bytes"; return false; }
       for (const Pattern& prev : p.patterns)
           if (!prev.id.empty() && prev.id == pat.id) {
               err = "pattern.id '" + pat.id + "' is already in use"; return false;
           }
   }
   ```

   In `project_to_json`, inside the pattern loop, immediately after `pj["track"] = pat.track;` add
   `if (!pat.id.empty()) pj["id"] = pat.id;  // WP-0179: omitted when unnamed → legacy bytes`.
3. `src/engine/ops.cpp` — in `apply_named`, add a branch for `"duplicate_pattern"` immediately after the
   `clear_pattern_length` branch (before the final `} else {` that sets `unknown op`):

   ```cpp
   } else if (name == "duplicate_pattern") {  // WP-0179: id rules come from the model parse below
       if (!check_keys(op_json, {"src", "new_id"}, err)) return false;
       std::size_t src_idx = 0;
       if (!arg_index(op_json, "src", j["patterns"].size(), src_idx, err) ||
           !has_string(op_json, "new_id", err)) return false;
       json dup = j["patterns"][src_idx];    // deep copy of the whole pattern object (notes included)
       dup["id"] = op_json.at("new_id");     // empty / taken ids are refused by the parse below
       j["patterns"].push_back(std::move(dup));  // appended: existing indices never shift
   ```

4. `src/engine/ops.cpp` — in `diff_projects`, turn the existing single-statement loop over shared patterns (the
   one that returns `unsupported("patterns[" + std::to_string(i) + "].track")`) into a braced loop that checks
   `track` first and then the id, keeping the same message shape:

   ```cpp
   static const json kNoId = std::string();  // canonical default of pattern id (WP-0179): key omitted
   for (std::size_t i = 0; i < std::min(patterns_a.size(), patterns_b.size()); ++i) {
       if (patterns_a[i].at("track") != patterns_b[i].at("track"))
           return unsupported("patterns[" + std::to_string(i) + "].track");
       if (field_or(patterns_a[i], "id", kNoId) != field_or(patterns_b[i], "id", kNoId))
           return unsupported("patterns[" + std::to_string(i) + "].id");
   }
   ```

   Nothing else in `diff_projects` changes: b's extra patterns already carry their own `id` inside the
   `add_pattern` JSON.
5. `tests/test_main.cpp` — add `static void test_duplicate_pattern()` (call it from `main`, e.g. after
   `test_sample_index();`) with **at least 24 CHECKs**, using the existing per-process scratch helpers
   `tmp_dir()` / `tmp_file(...)` (WP-0160) — never a fixed `/tmp/op_*` path. Cover at least: parse stores the id
   and canonical save writes it first among the pattern keys; canonical save omits `id` when unnamed; each of the
   four parse errors with its exact message, plus "id is checked before swing" precedence on a doubly-invalid
   pattern; 64-byte id accepted / 65 rejected; duplicate ids in one file rejected with
   `pattern.id 'A' is already in use`; `duplicate_pattern` appends (size +1, source untouched, no clip added) and
   the copy equals the source field-by-field (`track`, `swing`, `length_beats`, every note's `t`/`dur`/`note`/`vel`)
   except `id`; the resulting `project_bytes` equal those of a hand-built expected project; a copy of a bare
   pattern gains no `swing`/`length_beats`; `History` apply/undo/redo byte-exactness and that a rejected apply
   changes neither the bytes nor `undo_depth()`; every op rejection with its exact message via `rejected_unchanged`
   (missing `src`, `src: 0.0`, `src: "0"`, `src: -1`, `src` out of range, missing `new_id`, `new_id` number/`null`/bool,
   unknown key, keys-before-`src` and `src`-before-`new_id` ordering, empty `new_id`, 65-byte `new_id`, taken id);
   `add_pattern` with a fresh id accepted and with a duplicate id rejected; rendering before/after a duplicate is
   sample-identical (use a project whose only clip references pattern 0); `diff_projects` emits exactly one
   `add_pattern` carrying the copy's id and never mentions `duplicate_pattern`, its journal replayed through
   `apply_journal` reproduces the target bytes, equal projects give an empty journal, the reverse direction is a
   single `remove_pattern`; and an id change on a shared pattern returns false with
   `unsupported difference: patterns[0].id` (with `patterns[0].track` reported first when both differ).

## Acceptance criteria

- [ ] Everything above implemented exactly; **no new golden, no fixture touched**: every existing golden passes
      `bash harness/run_goldens.sh` unchanged.
- [ ] `opd apply <proj> <journal> -o out` with `{"op":"duplicate_pattern","src":0,"new_id":"Copy"}` appends the
      deep copy; rejections exit **5** with `error: apply: line N: duplicate_pattern: <reason>` and write nothing;
      a bad id inside a `.opj` fails `opd render` with exit **4** and the spec'd message; `opd diff` of a project
      and its duplicate emits one `add_pattern`, and an id-only difference exits **5** with
      `error: diff: unsupported difference: patterns[0].id`.
- [ ] Unit tests: ≥ 24 new CHECKs, all pass; `build/op_tests` ends `ALL PASS`.
- [ ] Standard gates green: build with **0 warnings** (`-Werror` default, `-j4` max), `op_tests`,
      `harness/run_goldens.sh`, `harness/test_cli_errors.sh`, `harness/test_glue_scripts.sh`, and a sanitizer
      build (`-DDZ_SANITIZE=ON`) of `op_tests`.
- [ ] ADR-0004 respected: no clock, no randomness, no new libm use (there is no arithmetic in this WP at all).
- [ ] `docs/spec/project-model.md` and `docs/spec/operations.md` carry the WP-0179 sections as provided by the
      runner (commit them with your change; do not rewrite them).

## Out of scope — do NOT touch

- `ROADMAP.md` (the elephant adds the row at ship time), golden fixtures (`tests/fixtures/`, read-only, add none),
  `src/app/`, `ai-sidecar/`, `docs/adr/`, `docs/spec/` other than committing the two provided sections.
- `CMakeLists.txt`, `src/cli/main.cpp` (no CLI subcommand or usage-text change — the op is applied through the
  existing `opd apply`), `src/engine/render.*`, `src/engine/midi.*`, `src/engine/lint.*`, `src/doc/`,
  `src/engine/ops.hpp` (the public API does not change: no new free function).
- Do **not** add a rename operation (`set_pattern_id`), id-based addressing (`"src": "Kick A"` is invalid — `src`
  is an integer index), pattern sorting, or automatic ids like `"pattern_0"`.
- Do **not** emit `duplicate_pattern` from `opd diff`, do not re-implement the id rules inside `ops.cpp`, and do
  not touch clips, note order, or any existing validation message.

## Git

- Branch `wp-0179-duplicate-pattern` from `main`. Commit: `WP-0179: pattern ids + duplicate_pattern op (deep copy under a unique name, undoable, diff as pattern add)`.
  Stage explicit paths only (never `git add -A` / `git add .`):
  `src/engine/model.hpp src/engine/model.cpp src/engine/ops.cpp tests/test_main.cpp docs/spec/project-model.md docs/spec/operations.md TASKS/WP-0179-duplicate-pattern.md`.
  Include this task file with the handoff filled.
- **Do not push or merge.**

---
## Handoff report — completed by the agent
- **What changed:**
  - `src/engine/model.hpp` (+1): `std::string id;` added to `struct Pattern` right after `int track`, default `""` = unnamed.
  - `src/engine/model.cpp` (+14): in `project_from_json`'s pattern loop, the four WP-0179 id rules (not-a-string / empty / >64 bytes / duplicate-of-an-earlier-pattern) inserted exactly between the `pattern.track out of range:` check and the `// Pattern swing (WP-0166 …` comment, first failure wins, spec strings verbatim; uniqueness scans `p.patterns` so patterns are checked in array order. In `project_to_json`, `if (!pat.id.empty()) pj["id"] = pat.id;` after `pj["track"]` — omitted when unnamed, alphabetical dump puts it first among the pattern keys.
  - `src/engine/ops.cpp` (+14/-1): new `duplicate_pattern` branch in `apply_named` immediately after `clear_pattern_length` (strict keys `{src,new_id}` → `arg_index("src")` → `has_string("new_id")` → append `json dup = j["patterns"][src_idx]` with `dup["id"] = new_id`; empty/taken ids refused by the existing save→parse validation, no second ruleset). In `diff_projects`, the shared-pattern loop is now braced and checks `patterns[i].track` then — immediately after, same ascending `i` — `field_or(…, "id", kNoId)` differences as `unsupported difference: patterns[i].id`. No other file touched; no CLI/CMake/golden change.
  - `tests/test_main.cpp` (+260): `test_duplicate_pattern()` (called from `main` after `test_sample_index();`) — **55 CHECKs** (`PASS dup.*`), all through the per-process `tmp_dir()/tmp_file()` scratch helpers.
  - `docs/spec/project-model.md`, `docs/spec/operations.md`: committed exactly as installed by the runner (unmodified).
- **How verified:** (all on branch `wp-0179-duplicate-pattern`, this machine = linux-aarch64 target)
  - Build, zero warnings with `-Werror` (`DZ_WERROR:BOOL=ON` in the cache; engine sources touched and recompiled — grep of the build log for warning/error found none):
    ```
    $ cmake --build build -j4        # after touch src/engine/model.{cpp,hpp} src/engine/ops.cpp
    [100%] Built target op_tests     # (no warning lines; grep -icE "warning|error" → 0)
    ```
  - Unit tests — `ALL PASS`, 55 of them new:
    ```
    $ ./build/op_tests | grep -E "^FAIL|ALL PASS|FAILURES"
    ALL PASS (0 failures)
    $ ./build/op_tests | grep -c "^PASS dup\."
    55                                  # (915 PASS lines total in the binary)
    ```
    Coverage: parse stores id / unnamed stays `""`; canonical save emits `"id"` first among pattern keys and omits it when unnamed; all four parse errors with exact strings (number, null, bool, object, array → `pattern.id must be a string`; `""` → `…must not be empty`; 65-byte → `…at most 64 bytes`; second `"A"` in one file → `pattern.id 'A' is already in use`); precedence both ways (`track` before `id`, `id` before `swing`); 64-byte id accepted; append semantics (size +1, source JSON untouched, no clip added); deep copy field-by-field (`track`/`swing`/`length_beats` and every note's `t`/`dur`/`note`/`vel`) with only `id` changed; result bytes == hand-built expected project bytes; bare-pattern copy gains no `swing`/`length_beats`; `History` apply→undo→redo byte-exact and rejected apply leaves bytes + `undo_depth()` unchanged; 14 op rejections via a `rejected_unchanged` helper with exact messages (missing `src`, `src:0.0`, `src:"0"`, `src:-1`, `src:2` OOB, missing `new_id`, `new_id` number/null/bool, unknown key, unknown-key-before-`src`, `src`-before-`new_id` ordering, empty / 65-byte / taken `new_id`); `add_pattern` fresh id accepted and duplicate refused (`add_pattern: pattern.id 'A' is already in use`); render before/after duplicate sample-identical; diff = exactly one `add_pattern` carrying `"id":"Copy"` and never `duplicate_pattern`, journal replay reproduces target bytes, equal projects → empty journal, reverse = single `remove_pattern index 2`; id-only difference → `unsupported difference: patterns[0].id`, present-vs-absent on pattern 1 too, `track` reported first when both differ.
  - Goldens unchanged (no new golden, no fixture touched):
    ```
    $ bash harness/run_goldens.sh | tail -2
    PASS negative-control (corrupt golden rejected)
    run_goldens: 11 fixtures, 0 failures
    ```
  - CLI exit-code contract suite still green: `bash harness/test_cli_errors.sh` → `test_cli_errors: 0 failures`.
  - Glue-script e2e (`UV_CACHE_DIR=/home/shubhankar_dgx/qwen-scratch/.cache/uv`): `bash harness/test_glue_scripts.sh` → `test_glue_scripts: 0 failures`.
  - Sanitizers (ASAN+UBSAN, `-DDZ_SANITIZE=ON` in `build-san`): `cmake --build build-san -j4 --target op_tests && ./build-san/op_tests` → `ALL PASS (0 failures)`, no sanitizer errors.
  - CLI acceptance, real transcripts (scratch dir, removed afterwards; base.opj = 1 sine track, pattern `{track:0,id:"A",swing:0.25,notes:[{t:0,dur:.25,note:60,vel:.5}]}`, one clip on pattern 0):
    ```
    $ echo '{"op":"duplicate_pattern","src":0,"new_id":"Copy"}' > dup.opjl
    $ ./build/opd apply base.opj dup.opjl -o out.opj; echo $?
    apply: 1 op(s) -> out.opj
    apply_exit=0          # patterns[1] = {"id":"Copy","notes":[{"dur":0.25,"note":60,"t":0.0,"vel":0.5}],"swing":0.25,"track":0}
    ```
    Rejection (line 2 taken id) exits **5**, names the line, writes nothing:
    ```
    $ ./build/opd apply base.opj bad.opjl -o rej.opj; echo $?      # stderr below
    reject_exit=5
    ls: cannot access '.tmp-wp0179/rej.opj': No such file or directory
    error: apply: line 2: duplicate_pattern: pattern.id 'A' is already in use
    ```
    Bad id inside a `.opj` fails `opd render` with exit **4** and the spec'd message, no WAV written:
    ```
    $ ./build/opd render badid.opj -o render_out.wav; echo $?      # pattern id set to ""
    render_exit=4
    ls: cannot access '.tmp-wp0179/render_out.wav': No such file or directory
    error: parse: pattern.id must not be empty
    ```
    `opd diff` of a project and its duplicate — one `add_pattern`, no `duplicate_pattern`:
    ```
    $ ./build/opd diff base.opj out.opj -o d.opjl; echo $?
    diff: 1 op(s) -> .tmp-wp0179/d.opjl
    diff_exit=0
    {"op":"add_pattern","pattern":{"id":"Copy","notes":[{"dur":0.25,"note":60,"t":0.0,"vel":0.5}],"swing":0.25,"track":0}}
    ```
    id-only difference exits **5** with the spec'd line, writes nothing:
    ```
    $ ./build/opd diff base.opj renamed.opj -o d2.opjl; echo $?
    iddiff_exit=5
    ls: cannot access '.tmp-wp0179/d2.opjl': No such file or directory
    error: diff: unsupported difference: patterns[0].id
    ```
    Render identity at CLI level too (the WP promises no audio change):
    ```
    $ ./build/opd render base.opj -o a.wav --seed 42 && ./build/opd render out.opj -o b.wav --seed 42 && cmp a.wav b.wav
    rendered 192000 frames @48000 Hz -> a.wav (seed=42)
    rendered 192000 frames @48000 Hz -> b.wav (seed=42)
    CLI render bit-identical before/after duplicate: OK
    ```
  - Other CI test binaries re-run for safety: `op_loudness_tests`, `op_normalize_tests`, `op_lint_tests`, `op_peaks_tests` → all PASS.
- **Metrics:** +288 / −1 lines across the 4 interface files + tests (model.hpp +1, model.cpp +14, ops.cpp +14/−1, test_main.cpp +260); 55 new CHECKs; no new golden, no fixture touched; no clock/randomness/libm added (no arithmetic at all).
- **PROPOSE / BLOCKED:** none. (The two `docs/spec/` sections are committed verbatim as provided by the runner; nothing outside WP-0179 was touched.)

