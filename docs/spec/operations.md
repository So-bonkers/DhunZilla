# Spec: Model operations, journal & undo/redo (`docs/spec/operations.md`, v1)

Status: normative from WP-0135. Owner of this spec: orchestrator (Claude). Implementers do not
change it; propose changes in a handoff report.

## Why
AGENTS.md: *every project mutation is a serializable operation applied to the project model.*
The future GUI, the scripting/copilot API and the CLI all become thin layers that build
operations. Undo/redo, autosave journals and "the AI edited your project" diffs fall out of this.

## Operation = one JSON object
```jsonc
{"op": "<name>", ...arguments}     // keys other than those listed for <name> → reject
```
Indices are 0-based positions in the current project's arrays.

| op | arguments | effect |
|---|---|---|
| `set_bpm` | `bpm` | `Project::bpm` |
| `set_length_bars` | `length_bars` | `Project::length_bars` |
| `add_track` | `track` (same JSON shape as an element of `tracks` in .opj) | append |
| `remove_track` | `index` | erase; **reject if any pattern references it** (`"track in use"`) |
| `set_track_gain` | `index`, `gain_db` | |
| `set_track_pan` | `index`, `pan` | |
| `set_track_mute` | `index`, `mute` (bool) | |
| `set_track_solo` | `index`, `solo` (bool) | |
| `set_master_gain` | `gain_db` | `Project::master_gain_db` |
| `rename_track` | `index`, `name` | |
| `add_pattern` | `pattern` (same shape as an element of `patterns`) | append |
| `remove_pattern` | `index` | erase; **reject if any clip references it** (`"pattern in use"`) |
| `add_note` | `pattern`, `note` (same shape as an element of `notes`) | append to that pattern |
| `remove_note` | `pattern`, `index` | erase |
| `add_clip` | `clip` (same shape as an element of `arrangement`) | append |
| `move_clip` | `index`, `start_bar` | |
| `set_clip_gain` | `index`, `gain_db` | |
| `remove_clip` | `index` | erase |
| `add_attachment` | `path`, `kind`, `producer` | same rules as `op::add_attachment` |

Index semantics after erase: later elements shift down by one (plain `vector::erase`).
**Pattern/track indices held by other elements are NOT rewritten** — that is why removal of a
referenced track/pattern is rejected instead of cascading.

## Apply semantics (normative)
`apply(project, operation)` is **atomic**:
1. Copy the project. Mutate the copy.
2. Validate the copy by serializing it with exactly the code path `save_project` uses and parsing
   the result with exactly the code path `load_project` uses (no second set of validation rules).
3. On any failure (unknown op, unknown/missing/mistyped argument, index out of range, in-use
   rejection, validation failure) the original project is **unchanged** and the error string is
   one line: `<op>: <reason>`.
4. On success the copy replaces the original.

Determinism: apply uses no clock, no randomness, no filesystem access (except none — attachments
are not checked for existence, same as today).

## Journal (`.opjl`) = JSON Lines
One operation per line, UTF-8, `\n`-terminated; blank lines and lines starting with `#` ignored.
Applying a journal is **all-or-nothing**: if line N fails, nothing is written and the error names
the 1-based line number: `line N: <op>: <reason>`.

## Undo / redo
`History` holds two stacks. Implementation: **whole-project snapshots** (boring, obviously correct;
projects are small). Cap 256 undo entries; the oldest is dropped beyond that.
- `apply(p, op)`: on success push the pre-apply snapshot to undo, **clear redo**.
- `undo(p)`: if undo non-empty, push current to redo, restore top of undo. Returns false if empty.
- `redo(p)`: mirror image.
- Failed applies do not touch either stack.

Invariant (tested): after any sequence of successful applies, undoing all of them yields a project
whose `save_project` bytes equal the original's; redoing all yields the forward result's bytes.

## CLI
```
opd apply <proj.opj> <ops.opjl> [-o out.opj]
```
Default output overwrites the input (same convention as `import-midi` / `attach`). Prints
`apply: N op(s) -> <out>` on success. Exit codes follow the existing `fail()` mapping in `src/cli/main.cpp`: 2 usage, 3 file not found,
4 parse error (project, or journal line that is not a JSON object → code name `parse`), **5 operation
rejected** (code name `apply`; stderr `error: apply: line N: <op>: <reason>` — same exit as a
rejected `opd attach`).
On any failure the output file is not written.

## `opd diff` — journal that transforms one project into another (additive, WP-0146)
```
opd diff <a.opj> <b.opj> -o <edits.opjl>
```
Writes a journal such that `opd apply a.opj edits.opjl -o c.opj` yields a `c.opj` **byte-identical** to
`save_project(b)`. No new operations exist; only differences the operations above can express are
supported. Engine: `bool op::diff_projects(const Project& a, const Project& b,
std::vector<nlohmann::json>& ops, std::string& err)` in `engine/ops.hpp` (ops cleared first; empty on
failure). "Equal" means equal canonical JSON (`project_to_json`), i.e. equal saved bytes.

**Unsupported differences** (no operation can change them). The first one found, checking in exactly this
order, is reported as `unsupported difference: <field>`: `format_version`, `sample_rate`, `seed`,
`beats_per_bar`, `tempo_map` (any difference), then for i ascending `tracks[i].voice` (i < min track
count), `patterns[i].track` (i < min pattern count), then for i ascending `arrangement[i].pattern` and
`arrangement[i].repeats` (i < min clip count), then `attachments` (a's attachment list must be a prefix of
b's — operations can only append). Anything else that differs is supported.

**Output**: JSON Lines, one `nlohmann::json::dump()` (compact, keys alphabetically sorted — so `"op"`
sorts among the arguments, e.g. `{"gain_db":-3.0,"op":"set_master_gain"}`) per operation, `\n`
terminated, no comments/blank lines. Values come from the canonical JSON of `b`. Identical projects →
a zero-byte file (exit 0). Operations are emitted in this fixed order (output is byte-stable):
1. `remove_clip` for indices `size(a.arrangement)-1` down to `size(b.arrangement)` (descending);
2. `remove_pattern` likewise (descending, a's tail beyond b's pattern count);
3. `remove_track` likewise;
4. `set_bpm`, `set_length_bars`, `set_master_gain` (each only if different);
5. for track i = 0.. (i < min track count), only those that differ, in this order: `rename_track`,
   `set_track_gain`, `set_track_pan`, `set_track_mute`, `set_track_solo` (mute/solo absent = false);
6. `add_track` for b's extra tracks, ascending (`track` = b's canonical track JSON);
7. for pattern i = 0.. (i < min pattern count): let k = length of the longest common prefix of a's and b's
   note lists; `remove_note` for a's note indices `size-1` down to k (descending), then `add_note` for b's
   notes k.. ascending (`note` = b's canonical note JSON);
8. `add_pattern` for b's extra patterns, ascending;
9. for clip i = 0.. (i < min clip count): `move_clip` if `start_bar` differs, then `set_clip_gain` if
   `gain_db` differs (absent = 0.0);
10. `add_clip` for b's extra clips, ascending;
11. `add_attachment` for b's attachments beyond a's count, ascending (`producer` always present, `""` if empty).

Finally `diff_projects` applies its own journal to a copy of `a` and returns false with
`unsupported difference: (journal does not reproduce target)` if the result is not equal to `b` (safety
net; unreachable for supported inputs).

**CLI**: prints `diff: N op(s) -> <out>` on success. `-o` is required. Exit codes follow `fail()`:
2 usage (missing args, missing `-o`, unknown arg), 3 either input file not found, 4 either project fails
to parse (`parse`), **5 unsupported difference** (code name `diff`; stderr
`error: diff: unsupported difference: <field>`), 5 write failure (`write`). On any failure the output
file is not created or modified.
## Clip operations — additive (WP-0147)
`add_clip`, `move_clip`, `set_clip_gain` and `remove_clip` already exist (table above). Four more
ops complete arrangement editing. All follow the Apply semantics above unchanged (copy → JSON edit →
save→parse validation → commit) and the same strict-argument rules (`check_keys`, missing/mistyped
argument rejection, integer-only indices). `<n>` below = number of clips in `arrangement` (0 when the
key is absent). Error strings are `<op>: <reason>`.

| op | arguments | effect |
|---|---|---|
| `set_clip_repeats` | `index`, `repeats` | `arrangement[index].repeats = repeats` |
| `set_clip_pattern` | `index`, `pattern` | `arrangement[index].pattern = pattern` |
| `duplicate_clip` | `index`, `start_bar` | insert a copy of `arrangement[index]` at position `index+1` with `start_bar` replaced; all other fields (`pattern`, `repeats`, `gain_db`) copied |
| `reorder_clip` | `index`, `to` | remove `arrangement[index]` and re-insert it so that it ends at position `to` (`to` is the final index, `0 <= to < n`); other clips keep their relative order |

Rules:
- `index` must be an integer in `[0, n)`, else `<op>: 'index' out of range` (non-integer incl. `2.0`:
  `<op>: 'index' must be an integer`; absent: `<op>: missing argument 'index'`).
- `set_clip_repeats`: `repeats` must be a JSON integer (`3.0` → `set_clip_repeats: 'repeats' must be an
  integer`). The range `[1..4096]` is NOT re-implemented: an out-of-range value is rejected by the model
  parse with its existing message, giving `set_clip_repeats: clip.repeats out of range [1..4096]`. Integers outside the 32-bit `int` range (e.g. `4294967297`) must get the same message, never wrap silently.
- `set_clip_pattern`: `pattern` must be a JSON integer (`'pattern' must be an integer`). Range is
  enforced by the model parse: `set_clip_pattern: clip.pattern out of range: <value>` (value printed as the JSON integer; integers outside 32-bit `int` range get `clip.pattern out of range: <value>` too, never wrap). The referenced
  pattern's track is not otherwise checked.
- `duplicate_clip`: `start_bar` must be a number (`'start_bar' must be a number`); `>= 0` enforced by
  the model parse (`clip.start_bar must be a finite number >= 0`). Duplicating at the same `start_bar`
  is allowed (clips may overlap).
- `reorder_clip`: `to` validated like `index` (`'to' out of range`, etc.). `to == index` is a valid
  no-op that still counts as a successful operation (and pushes an undo snapshot). Declaration order
  is render accumulation order (project-model.md §Arrangement), so this op can change float
  summation order by design.
- Absent `arrangement` has `n == 0`, so every one of these ops fails with `'index' out of range`.
- Undo/redo: no new mechanism. `History` already snapshots the whole project, so each of these ops is
  undone/redone by `History::undo`/`redo` byte-exactly (invariant above).
- `set_clip_gain` setting `0` makes the saved clip omit `gain_db` (existing canonical-save rule);
  `duplicate_clip` of a clip without `gain_db` yields a copy without `gain_db`.

## Step sequencer — additive (WP-0154)
A **step grid** is a *view* over the existing notes of one pattern; no model field is added and no
new file format exists. Everything is expressed with ordinary `Note`s.

**Definitions.** One step = 1/4 beat (a 16th note), so 16 steps = one 4/4 bar. Step `s` (integer
`s >= 0`) is at beat `s * 0.25`. A note **is on step `s`** iff its `t` equals `s * 0.25` *exactly*
(double equality; `s * 0.25` is exactly representable, so `1.0`, `0.25` and `3.75` qualify while
`0.3` or `0.30000000000000004` or `1.0000001` do not). A missing `t` counts as `0.0` (the model's
default). Pitch, `dur` and `vel` of the note are irrelevant to *being on a step*: a step is **on**
iff at least one note is on it. A note is **on the grid** iff it is on some step (`t >= 0` and
`t * 4` is an integer, i.e. `std::floor(t * 4.0) == t * 4.0`); every other note is **off-grid**.
Off-grid notes are never read, changed, moved or removed by any step operation or by `step_grid`.

**Default step note** (created by `set_step` when the step is off): `{"t": s*0.25, "dur": 0.25}` —
no `"note"` key (pitch absent → the voice's own pitch/sample, which is what drum tracks want) and no
`"vel"` key (velocity 1.0), unless `pitch` / `velocity` arguments are given.

### Operations
Both follow Apply semantics unchanged (copy → JSON edit → save→parse validation → commit) and the
strict-argument rules (`check_keys`, missing/mistyped rejection, integer-only indices, `<op>: <reason>`
errors). Neither may create a `notes` key on a pattern that lacks one as a side effect of a failed or
no-op call (read with `const` access).

| op | arguments | effect |
|---|---|---|
| `set_step` | `pattern`, `step`, `on`, optional `velocity`, optional `pitch` | see below |
| `clear_steps` | `pattern` | erase every on-grid note of the pattern |

**`set_step`** — argument validation, in exactly this order (first failure wins):
1. keys: only `op, pattern, step, on, velocity, pitch` → else `unknown argument '<key>'`;
2. `pattern`: integer in `[0, patterns.size())` → `missing argument 'pattern'` / `'pattern' must be an
   integer` / `'pattern' out of range`;
3. `step`: integer in `[0, 4096)` → `missing argument 'step'` / `'step' must be an integer` (also for
   `2.0`) / `'step' out of range` (negative, `>= 4096`, or beyond 64 bits);
4. `on`: boolean → `missing argument 'on'` / `'on' must be a boolean`;
5. `velocity` (if present): JSON number → `'velocity' must be a number`; its range `(0, 1]` is NOT
   re-implemented: out of range is rejected by the model parse, giving `set_step: note.vel must be in
   (0, 1]`;
6. `pitch` (if present): JSON integer → `'pitch' must be an integer`; must be in `[0, 127]` →
   `'pitch' out of range` (`-1` is rejected even though the model treats -1 as "absent");
7. if `on` is `false` and `velocity` or `pitch` is present → `'velocity' not allowed when 'on' is
   false` (checked for `velocity` first, then `'pitch' not allowed when 'on' is false`).

Effect, with `t = step * 0.25`:
- **`on: true`, no note is on the step:** append the default step note (with `"note": pitch` and/or
  `"vel": velocity` if given) at the **end** of `notes`. Other notes keep their order and are not
  re-sorted.
- **`on: true`, one or more notes are on the step:** only the note with the **lowest index** among
  them is touched: `velocity` (if given) replaces its `vel`, `pitch` (if given) replaces its `note`;
  its `t` and `dur` are never changed (a long note stays long). Arguments not given leave the
  field as it was (velocity is *not* reset to 1.0). No note is added, removed or reordered; other
  notes on the same step are untouched. With neither `velocity` nor `pitch` the state does not change.
- **`on: false`:** erase **every** note on the step (whatever pitch/dur/vel); remaining notes keep their
  relative order. If none exists the state does not change.

**Idempotence:** applying the same `set_step` twice leaves the project (canonical save bytes) exactly as
after the first application. A call that leaves the state unchanged (turning an on step on again with
no arguments, turning an off step off) is still a **successful operation** and `History` pushes an undo
snapshot for it (same rule as `reorder_clip` with `to == index`).

**`clear_steps`:** erases every on-grid note of `patterns[pattern]` (any `t = k*0.25`, `k >= 0`, even
beyond step 4095), preserving the order of the rest. Off-grid notes are untouched. Empty or all-off-grid
pattern: success, no change. Validation order: keys (`op, pattern` only), then `pattern` as for
`set_step`. Idempotent.

Undo/redo: no new mechanism — `History` snapshots the whole project, so both ops undo/redo byte-exactly.

### Engine view and CLI
```cpp
// engine/ops.hpp, namespace op
std::string step_grid(const Pattern& pattern, int steps);
```
Returns a string of exactly `max(steps, 0)` characters; character `i` is `'x'` if step `i` is on, else
`'.'`. Notes at steps `>= steps` and all off-grid notes are ignored. `steps <= 0` → `""`.
Pure function (no I/O, no allocation beyond the result).

```
opd steps <proj.opj> <pattern-index> [--steps N]
```
Prints `step_grid(pattern, N)` followed by one `\n` to stdout and nothing else; `N` defaults to 16.
Argument errors are `error: usage: <detail>` on stderr, exit 2:
- fewer than two positional arguments → `steps needs <proj.opj> <pattern-index>`;
- `<pattern-index>` not entirely decimal digits (`x`, `-1`, `1.0`, empty) → `bad pattern index '<arg>'`;
- `--steps` value not entirely decimal digits or outside `[1, 256]` → `bad --steps value '<arg>'`;
  `--steps` without a value or any other extra argument → `unknown arg '<arg>'`.
Then: project file missing → exit 3 (`not_found`); fails to parse → exit 4 (`parse`); index `>=` pattern
count → exit 2, `error: usage: pattern index <n> out of range (project has <m> patterns)`.
Option syntax is checked before the project is loaded. Example: a pattern with kick notes at beats
0, 1, 2, 3 prints `x...x...x...x...`.
## Loop region operations — additive (WP-0155)
Two more operations, same Apply semantics (copy → JSON edit → save→parse validation → commit) and strict
argument rules (`check_keys`, missing/mistyped rejection). Error strings are `<op>: <reason>`.

| op | arguments | effect |
|---|---|---|
| `set_loop` | `start_bar`, `end_bar` (numbers) | sets `Project::loop` (replacing any existing loop) |
| `clear_loop` | none | removes `Project::loop`; succeeds as a no-op when none is set |

- `set_loop`: absent argument → `set_loop: missing argument 'start_bar'` / `'end_bar'` (start checked
  first); non-number → `set_loop: 'start_bar' must be a number` / `'end_bar' must be a number`. Range rules
  are NOT re-implemented: they come from the model parse (project-model.md §Loop region), e.g.
  `set_loop: loop.end_bar must be greater than loop.start_bar`,
  `set_loop: loop.end_bar must be <= length_bars`,
  `set_loop: loop.start_bar must be a finite number >= 0`.
- `clear_loop` with any argument → `clear_loop: unknown argument '<key>'`.
- A successful `clear_loop` on a project without a loop still pushes an undo snapshot (like `reorder_clip`
  with `to == index`).
- Undo/redo: no new mechanism; byte-exact per the invariant above.

### `opd diff` and loops (WP-0155 amendment)
`loop` is expressible, so it is never an "unsupported difference". Two emission steps are added to the fixed
order of §`opd diff`, and only when `a.loop != b.loop` (absent counts as a distinct value):
- step **3b**, after step 3 (`remove_track`) and before step 4: `clear_loop` if `a` has a loop;
- step **4b**, after step 4 (`set_bpm`, `set_length_bars`, `set_master_gain`) and before step 5: `set_loop`
  (`start_bar`, `end_bar` from b's canonical JSON) if `b` has a loop.
This order is always valid (the old loop is gone before the length changes; the new one is set after).
Projects without loops produce exactly the same journals as before.
