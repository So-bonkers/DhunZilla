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
count), `patterns[i].track` (i < min pattern count) and — since WP-0179, checked immediately after the track for
the same i — `patterns[i].id` (absent counts as "unnamed", so present-vs-absent on a shared pattern differs too),
then `attachments` (a's attachment list must be a prefix of
b's — operations can only append). Anything else that differs is supported. Since WP-0159 that includes every clip field: `arrangement[i].pattern` and
`arrangement[i].repeats` are no longer unsupported (they are expressed with `set_clip_pattern` and
`set_clip_repeats`).

**Output**: JSON Lines, one `nlohmann::json::dump()` (compact, keys alphabetically sorted — so `"op"`
sorts among the arguments, e.g. `{"gain_db":-3.0,"op":"set_master_gain"}`) per operation, `\n`
terminated, no comments/blank lines. Values come from the canonical JSON of `b`. Identical projects →
a zero-byte file (exit 0). Operations are emitted in this fixed order (output is byte-stable):
1. `remove_clip` for indices `size(a.arrangement)-1` down to `size(b.arrangement)` (descending);
1b. (WP-0159) only if `size(a.patterns) > size(b.patterns)`: for clip i = 0.. (i < min clip count),
    `set_clip_pattern` where `pattern` differs (`pattern` = b's value). Rationale: b's values are then
    `< size(b.patterns) < size(a.patterns)`, valid now, and it frees patterns about to be removed;
2. `remove_pattern` likewise (descending, a's tail beyond b's pattern count);
3. `remove_track` likewise;
4. `set_bpm`, `set_length_bars`, `set_master_gain` (each only if different);
5. for track i = 0.. (i < min track count), only those that differ, in this order: `rename_track`,
   `set_track_gain`, `set_track_pan`, `set_track_mute`, `set_track_solo` (mute/solo absent = false), then (WP-0181)
   `set_choke_group` if the track's `choke_group` differs (absent = none; `group` = b's value, `null` when b's is absent);
6. `add_track` for b's extra tracks, ascending (`track` = b's canonical track JSON);
7. for pattern i = 0.. (i < min pattern count): let k = length of the longest common prefix of a's and b's
   note lists; `remove_note` for a's note indices `size-1` down to k (descending), then `add_note` for b's
   notes k.. ascending (`note` = b's canonical note JSON), then (WP-0166)
   `set_pattern_swing` if the pattern's `swing` differs (absent = 0; `swing` = b's value, `0.0` when b's is absent);
8. `add_pattern` for b's extra patterns, ascending (b's canonical pattern JSON, which since WP-0179 carries its
   own `id` when it has one — this is how a duplicated pattern appears in a diff);
9. for clip i = 0.. (i < min clip count), in this order: `set_clip_pattern` if `pattern` differs and
   it was NOT already emitted in step 1b (i.e. `size(a.patterns) <= size(b.patterns)`; all of b's patterns
   exist after step 8), `set_clip_repeats` if `repeats` differs (`repeats` = b's value), `move_clip` if
   `start_bar` differs, then `set_clip_gain` if `gain_db` differs (absent = 0.0), then (WP-0178) ONE
   `set_clip_fades` if either fade differs (`fade_in_beats`/`fade_out_beats` = b's values, `0.0` when absent);
10. `add_clip` for b's extra clips, ascending;
11. `add_attachment` for b's attachments beyond a's count, ascending (`producer` always present, `""` if empty).

**Clip lists are compared position by position** (WP-0159). Clip i of a is turned into clip i of b by the
per-field ops above, and the tails are removed/added. No `duplicate_clip` or `reorder_clip` is ever
emitted: a duplicated or reordered clip simply shows up as per-position field changes plus an
added tail clip (correct, not minimal). Detecting inserts/moves is deliberately not attempted.
Pairs whose clip lists agree in every shared position's `pattern` and `repeats` produce exactly the same
journal bytes as before WP-0159.
Projects whose patterns all have equal `swing` produce exactly the same journals as before WP-0166.
Projects whose clips all have equal fades produce exactly the same journals as before WP-0178
(a clip pair where neither side has a fade key, or both sides carry identical values).

Projects whose tracks all have equal `choke_group` produce exactly the same journals as before WP-0181.

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
## Sampler trim operation — additive (WP-0156)
| op | arguments | effect |
|---|---|---|
| `set_sampler_trim` | `index`, `start_s`, `end_s` | set the slice of `tracks[index]`'s sampler voice (semantics: `docs/spec/project-model.md` §Sampler trim) |

Follows the Apply semantics unchanged (strict keys, copy → JSON edit → save→parse validation → commit; no second
ruleset). Argument checks, in this order, error strings `set_sampler_trim: <reason>`:
1. unknown key → `unknown argument '<key>'`;
2. `index` an integer in `[0, #tracks)` (existing `arg_index` messages: `'index' out of range`, etc.);
3. `start_s` required JSON number (`missing argument 'start_s'` / `'start_s' must be a number`);
4. `end_s` required (`missing argument 'end_s'`) and either a JSON number or `null`
   (anything else → `'end_s' must be a number or null`); `null` means "no end" (play to the end of the asset);
5. the track's voice must be a sampler → `track is not a sampler voice`;
6. the edit writes `voice.start_s = start_s` and `voice.end_s = end_s` (or removes `end_s` for `null`), then the
   model parse enforces ranges, giving e.g. `set_sampler_trim: voice.start_s must be a finite number >= 0`,
   `set_sampler_trim: voice.end_s must be a finite number > 0`,
   `set_sampler_trim: voice.end_s must be greater than voice.start_s`.
`{"start_s": 0, "end_s": null}` restores defaults, and the canonical save then omits both keys (byte-identical
to a never-trimmed project). Undo/redo: whole-project snapshots, byte-exact as for every op. `opd diff` does not
emit this op (see project-model.md).

## Synth parameters operation — additive (WP-0161)
| op | arguments | effect |
|---|---|---|
| `set_synth_params` | `index`, and any of `waveform`, `adsr`, `cutoff_hz`, `resonance` (at least one) | set synth-v1 parameters of `tracks[index]`'s synth voice (semantics and ranges: `docs/spec/project-model.md` §Synth voice v1) |

Follows the Apply semantics unchanged (strict keys, copy → JSON edit → save→parse validation → commit; no second
ruleset). Argument checks, in this order, error strings `set_synth_params: <reason>`:
1. unknown key → `unknown argument '<key>'`;
2. `index` an integer in `[0, #tracks)` (existing `arg_index` messages);
3. none of the four parameter keys present → `no synth parameters given`;
4. the track's voice must be a synth → `track is not a synth voice`;
5. each given key is written to the voice (`adsr` replaces the whole object: inner keys not given take their
   defaults); a JSON `null` value **removes** the key (= reset to default); then the model parse enforces types
   and ranges, giving e.g. `set_synth_params: voice.waveform must be sine|saw|square`,
   `set_synth_params: voice.cutoff_hz must be a number in [20, 20000]`.
Keys not mentioned are left as they are. Setting every param to `null` restores a never-edited voice and the
canonical save is then byte-identical to the original legacy bytes. Undo/redo: whole-project snapshots, byte-exact.
`opd diff` does not emit this op.

## Pattern length operations — additive (WP-0162)
Two more operations, same Apply semantics and strict-argument rules as above (`check_keys`, missing/mistyped
rejection, integer-only indices, `<op>: <reason>` errors). Semantics of the field: project-model.md §Explicit
pattern length.

| op | arguments | effect |
|---|---|---|
| `set_pattern_length` | `pattern`, `length_beats` | `patterns[pattern].length_beats = length_beats` |
| `clear_pattern_length` | `pattern` | removes `length_beats` from that pattern; no-op success when absent |

- `set_pattern_length` validation order: keys (`op, pattern, length_beats` only → `unknown argument '<key>'`);
  `pattern` integer in `[0, #patterns)` (`missing argument 'pattern'` / `'pattern' must be an integer` /
  `'pattern' out of range`); `length_beats` a JSON number (`missing argument 'length_beats'` /
  `'length_beats' must be a number`). The positivity/finiteness rule is NOT re-implemented; the model parse
  gives `set_pattern_length: pattern.length_beats must be a finite number > 0` for `0`, negatives, etc.
- `clear_pattern_length`: keys (`op, pattern` only), then `pattern` as above. A successful call on a pattern
  without the field still pushes an undo snapshot (like `clear_loop`).
- Undo/redo: whole-project snapshots, byte-exact per the invariant above.

### `opd diff` and pattern length (WP-0162 amendment)
`length_beats` is expressible, never an "unsupported difference". New emission step **7b**, after step 7 (note
edits) and before step 8 (`add_pattern`): for pattern i = 0.. (i < min pattern count), `set_pattern_length`
(`length_beats` = b's canonical value) if b has the field and a lacks it or its value differs, else
`clear_pattern_length` if a has it and b does not. Patterns added by step 8 carry their own `length_beats` in
the `pattern` JSON. Projects without the field produce exactly the same journals as before.

### `opd steps` default width (WP-0162 amendment)
When `--steps` is NOT given and the pattern has `length_beats`, the width is `ceil(length_beats * 4)` clamped
to `[1, 256]` (so `length_beats` 4 gives 16 steps, 8 gives 32, 0.5 gives 2). An explicit `--steps N` always
wins. Without `length_beats` the default stays 16. `step_grid` itself is unchanged.

## Pattern swing operation — additive (WP-0166)
| op | arguments | effect |
|---|---|---|
| `set_pattern_swing` | `pattern`, `swing` | set `patterns[pattern].swing` (semantics: `docs/spec/project-model.md` §Pattern swing) |

Follows the Apply semantics unchanged (strict keys, copy → JSON edit → save→parse validation → commit; no second
ruleset). Argument checks, in this order, error strings `set_pattern_swing: <reason>`:
1. unknown key → `unknown argument '<key>'`;
2. `pattern` an integer in `[0, #patterns)` (existing `arg_index` messages: `missing argument 'pattern'`,
   `'pattern' must be an integer` (also `1.0`), `'pattern' out of range`);
3. `swing` required JSON number: `missing argument 'swing'` / `'swing' must be a number`;
4. the edit writes `patterns[pattern].swing = swing`, then the model parse enforces the range, giving
   `set_pattern_swing: pattern.swing must be a number in [0, 0.75]` (range is NOT re-implemented in the op).
`swing: 0` restores the default and the canonical save then omits the key (byte-identical to a never-swung
project). Setting the value it already has is a successful op (pushes an undo snapshot). Undo/redo: whole-project
snapshots, byte-exact as for every op. `opd diff` emits this op (see §`opd diff` step 7).

## Sampler repitch operation — additive (WP-0167)
| op | arguments | effect |
|---|---|---|
| `set_sampler_root` | `index`, `root_note` | set (or clear) the root pitch of `tracks[index]`'s sampler voice (semantics: `docs/spec/project-model.md` §Sampler repitch) |

Follows the Apply semantics unchanged (strict keys, copy → JSON edit → save→parse validation → commit; no second
ruleset). Argument checks, in this order, error strings `set_sampler_root: <reason>`:
1. unknown key → `unknown argument '<key>'`;
2. `index` an integer in `[0, #tracks)` (existing `arg_index` messages);
3. `root_note` required (`missing argument 'root_note'`) and a JSON integer or `null`
   (anything else, including `60.5` and strings → `'root_note' must be an integer or null`);
4. the track's voice must be a sampler → `track is not a sampler voice`;
5. the edit writes `voice.root_note = root_note` (or removes the key for `null`), then the model parse enforces the
   range, giving `set_sampler_root: voice.root_note must be an integer 0..127` for e.g. `128` or `-1`.
`{"root_note": null}` restores defaults, and the canonical save then omits the key (byte-identical to a project
that never had one). Undo/redo: whole-project snapshots, byte-exact as for every op. `opd diff` does not emit
this op.



## Pattern duplicate operation — additive (WP-0179)
| op | arguments | effect |
|---|---|---|
| `duplicate_pattern` | `src`, `new_id` | append to `patterns` a deep copy of `patterns[src]` whose `id` is `new_id` |

Follows the Apply semantics unchanged (strict keys, copy → JSON edit → save→parse validation → commit; no second
ruleset). `<n>` below = number of patterns. Argument checks, in this order, error strings `duplicate_pattern: <reason>`:
1. unknown key → `unknown argument '<key>'` (only `op`, `src`, `new_id` are allowed);
2. `src` an integer in `[0, n)` — the existing `arg_index` messages: `missing argument 'src'`, `'src' must be an
   integer` (also for `0.0`), `'src' out of range` (negative or `>= n`; this is the "unknown source pattern" case);
3. `new_id` a required JSON string — `missing argument 'new_id'` / `'new_id' must be a string` (for numbers,
   `null`, booleans, objects and arrays);
4. the edit appends a deep copy of `patterns[src]` with `"id"` set to `new_id`, then the model parse enforces the
   id rules (project-model.md §Pattern ids), giving `duplicate_pattern: pattern.id must not be empty`,
   `duplicate_pattern: pattern.id must be at most 64 bytes`, `duplicate_pattern: pattern.id 'X' is already in use`.

Effect details (all normative):
- **Deep copy:** every field of the source pattern object is copied — `track`, `swing`, `length_beats` when
  present, and all notes with their `t`, `dur`, `note` and `vel` keys in order. Only `"id"` is replaced. A source
  without `swing`/`length_beats` yields a copy without them; a copy of an unnamed pattern gains exactly one key
  the source did not have.
- **The copy is appended:** it becomes `patterns[n]`. No existing pattern index shifts and no clip changes —
  unlike `duplicate_clip`, which inserts at `index+1`. The source pattern is left untouched, and duplicating
  never alters a single sample of an arrangement that does not reference the copy.
- **Taken id:** `new_id` equal to the id of any other pattern — including the source's own id → rejected with the
  parse message above. A project whose patterns are all unnamed can name its first copy freely; ids may contain
  spaces and non-ASCII (they are compared byte-for-byte).
- Undo/redo: no new mechanism. `History` snapshots the whole project, so the op is undone/redone byte-exactly —
  undoing restores the exact original bytes, including the absence of the extra pattern.

### `opd diff` and pattern ids (WP-0179 amendment)
`duplicate_pattern` is **never emitted** by `opd diff` (same rule as `duplicate_clip`/`reorder_clip`): when `b`
is `a` plus a duplicated pattern, the journal is exactly one step-8 `add_pattern` whose `pattern` JSON is b's
canonical copy (including its `id`), and applying that journal to `a` reproduces `save_project(b)` byte-exactly.
An id difference on a **shared** pattern index cannot be expressed — no operation renames a pattern — so it is an
unsupported difference: `patterns[i].id`, checked immediately after `patterns[i].track` for i ascending (first hit
reports; absent counts as "unnamed", so present-vs-absent differs too). Projects whose patterns all have equal ids
produce exactly the same journal bytes as before WP-0179.

## Choke group operation — additive (WP-0181)
| op | arguments | effect |
|---|---|---|
| `set_choke_group` | `index`, `group` | set (or clear) the choke group of `tracks[index]` (semantics: `docs/spec/project-model.md` §Sampler choke groups) |

Follows the Apply semantics unchanged (strict keys, copy → JSON edit → save→parse validation → commit; no second
ruleset). Argument checks, in this order, error strings `set_choke_group: <reason>`:
1. unknown key → `unknown argument '<key>'`;
2. `index` an integer in `[0, #tracks)` (existing `arg_index` messages);
3. `group` required (`missing argument 'group'`) and a JSON integer or `null` (anything else, including `1.5` and
   strings → `'group' must be an integer or null`);
4. the track's voice must be a sampler → `track is not a sampler voice` (also for `null`);
5. the edit writes `tracks[index].choke_group = group` (or removes the key for `null`), then the model parse enforces the
   range, giving `set_choke_group: track.choke_group must be an integer in [1, 16]` for e.g. `0` or `17`.
`{"group": null}` clears the group and the canonical save then omits the key (byte-identical to a track that never had
one). Setting the value it already has is a successful op (pushes an undo snapshot). Undo/redo: whole-project
snapshots, byte-exact as for every op. `opd diff` emits this op (see §`opd diff` step 5).

## Song-structure operations — additive (WP-0180)
| op | arguments | effect |
|---|---|---|
| `insert_bars` | `at_bar`, `count` | open `count` empty bars at bar `at_bar`: later material moves later |
| `delete_bars` | `at_bar`, `count` | remove bars `[at_bar, at_bar + count)`: later material moves earlier |

Both follow the Apply semantics unchanged (strict keys, copy → edit → save→parse validation → commit). Both
arguments are JSON **integers** (bars, `at_bar` 0-based); `4.0` is rejected. Let `a = at_bar`, `n = count`,
`b = a + n`, `L = length_bars`. Error strings are `<op>: <reason>`; checks run in exactly this order, first hit wins:
1. unknown key → `unknown argument '<key>'`;
2. `at_bar` then `count`: absent → `missing argument '<key>'`; not an integer → `'<key>' must be an integer`;
3. `at_bar` outside `[0, 1000000000]` → `'at_bar' out of range`;
4. `count` outside `[1, 4096]` → `'count' out of range [1..4096]`;
5. the project has a non-empty `tempo_map` → `tempo_map is not supported` (tempo points are not shifted; unsupported by design);
6. `insert_bars`: `a > L` → `'at_bar' out of range` (`a == L` appends); `delete_bars`: `b > L` → `range exceeds length_bars`;
7. `delete_bars`: the arrangement was non-empty and no clip would remain → `would remove every clip` (an empty
   arrangement means "every pattern once at bar 0", so deletion must never produce one);
8. the edited project goes through the model parse; deleting down to `L - n == 0` on a project with no clips gives
   `length_bars must be > 0`.

### `insert_bars` effect
- `length_bars += n`.
- Every clip with `start_bar >= a` gets `start_bar += n`. Clips with `start_bar < a` are unchanged even when they
  extend past `a` (never split).
- Loop region `[ls, le)` (when present): if `ls >= a` both bounds `+= n`; else if `le > a` only `le += n` (the loop
  grows); else unchanged.
- Tracks, patterns, notes, attachments, everything else: unchanged.

### `delete_bars` effect
Clip handling, evaluated per clip against the ORIGINAL values, arrangement order preserved:
1. `start_bar >= b` → kept with `start_bar -= n`;
2. `a <= start_bar < b` → removed;
3. `start_bar < a` → kept with `repeats` replaced by the number of statements `i` in `[0, repeats)` for which
   `start_bar + i * (pattern_extent(pattern) / beats_per_bar) < a` (computed in `double`; `pattern_extent` is
   the pattern's explicit `length_beats`, else `max(t + dur)`, in beats). `i = 0` always qualifies, so at least one
   statement stays. A kept statement that starts before `a` is never shortened, even if it ends after `a`.
   `gain_db`, `pattern` are unchanged.
Loop region `[ls, le)`: map each bound with `f(x) = x` for `x <= a`, `f(x) = a` for `a < x < b`, `f(x) = x - n` for
`x >= b`. If `f(le) <= f(ls)` the loop is removed (as `clear_loop`); otherwise it becomes `[f(ls), f(le))`.
Finally `length_bars -= n`.

Undo/redo: whole-project snapshots, byte-exact as for every op; a rejected op changes nothing. 

### `opd diff` and song structure (WP-0180)
`opd diff` never emits `insert_bars` / `delete_bars`; the results of these ops are expressed with the existing ops
(`set_length_bars`, clip and loop ops), exactly as §`opd diff` defines. It is a requirement that for any project `a`
and `b = a` after one of these ops, `diff_projects(a, b)` succeeds and its journal reproduces `b` byte-for-byte.

## Clip trim operation — additive (WP-0193a)

`{"op":"set_clip_trim","index":I,"offset_beats":X,"length_beats":Y}` — exactly these four keys; `index` is a
clip index (`'index' out of range` as for the other clip ops). `offset_beats` and `length_beats` are both
**required**; each is a JSON number or `null`. `null` clears that side (offset → 0, length → untrimmed: the
key is removed from the clip and the canonical save omits it).

Validation order: unknown key → `unknown argument 'x'`; `index`; then for `offset_beats` then `length_beats`:
absent → `missing argument 'offset_beats'` / `missing argument 'length_beats'`; neither number nor null →
`'offset_beats' must be a number or null` / `'length_beats' must be a number or null`. Range rules are NOT
duplicated in the op: the edited project goes through the normal save→parse validation, so a bad value fails
with the project-model parse error (see project-model.md §Clip trim). All errors surface as
`set_clip_trim: <message>`; a rejected op leaves the project unchanged. The op is undoable through `History`.

### `opd diff` and clip trim (WP-0193a amendment)
In step 9, right after the `set_clip_fades` emission for a clip position, emit ONE `set_clip_trim` if
`offset_beats` or `length_beats` differs between a and b in their canonical JSON (an absent key counts as
absent; there is no `0` equivalence beyond canonical omission). The op carries b's canonical values and
`null` for a key b's clip does not have: `{"index":0,"length_beats":2.0,"offset_beats":1.0,"op":"set_clip_trim"}`.
Clips added by `add_clip` carry their trim keys inside the clip JSON. Pairs where no shared clip differs in
trim produce exactly the same journals as before.

## Arrangement clip view — `opd clips` — additive (WP-0199)

```
opd clips <proj.opj>
```

Read-only, deterministic: loads the project and prints **one JSON entry per arrangement clip in model
order** (the order of the `arrangement` array — never sorted by position). It writes nothing, modifies no
project, and creates no files. Exactly one positional argument, no options; errors print nothing on
stdout: missing/extra argument or an option-looking token → exit 2 with detail
`clips needs exactly <project.opj>`; file missing → exit 3 (`not_found`, detail = the path); project fails
to parse → exit 4 (`parse`, detail = the loader's message).

Engine API (`engine/render.hpp`, namespace `op`): `std::string clips_json(const Project& p);` — a pure
function returning exactly the text `opd clips` prints: a JSON array dumped with nlohmann `dump(2)`
(two-space indent, object keys sorted alphabetically) plus **exactly one trailing `\n`**. Doubles are
serialized by nlohmann exactly as stored (shortest round-trip; integral-valued doubles keep a `.0`);
`index`, `pattern`, `repeats`, `start_sample`, `end_sample` are JSON integers. An empty or absent
`arrangement` yields `[]\n` — the implicit legacy placements (every pattern once at bar 0) are **not**
synthesized by this view.

Each entry has exactly these keys and no others:

| key | value |
|---|---|
| `index` | the clip's position in `arrangement` (0-based integer) |
| `pattern` | the referenced pattern index (`clip.pattern`) |
| `pattern_name` | the referenced pattern's `id` string — **omitted entirely when the pattern has no id** (never invented as `""`, `"Pattern 0"` or `null`) |
| `start_bar` | stored `clip.start_bar` (number, bars) |
| `repeats` | stored `clip.repeats` (integer, verbatim even when placement clamps it — see below) |
| `gain_db` | stored `clip.gain_db` (number; always present, default 0.0) |
| `fade_in_beats` / `fade_out_beats` | stored fade values (numbers; always present, default 0.0) |
| `start_beats` | `clip.start_bar * double(beats_per_bar)` |
| `end_beats` | `start_beats + repeats * extent` (see geometry) |
| `start_sample` / `end_sample` | the renderer's own beat→sample mapping applied to `start_beats` / `end_beats` |

**Geometry (normative — identical to the renderer's placement rule in `for_each_placement`):**
`extent = op::pattern_extent(pattern)` (explicit `length_beats` when present, else
`max(note.t + note.dur)`, `0.0` for a pattern without notes);
`end_beats = start_beats + clip.repeats * extent` computed as a plain product (an empty pattern has
`extent == 0.0`, so `end_beats == start_beats` and both samples are equal for ANY `repeats`, while the entry
still reports the stored `repeats`; do not add a special case for it). The loader guarantees
`clip.pattern` is a valid index and `clip.repeats` is in [1, 4096], so the CLI never sees anything else;
for a hand-built `Project` passed straight to `clips_json` there is no clamping or validation:
`repeats` 0 prints `end == start` and a negative `repeats` prints `end < start` (pure arithmetic), and an
out-of-range `clip.pattern` is the caller's error (undefined, not tested). Samples use the renderer's own tempo map (`build_tempo_map(p, p.sample_rate)` /
`TempoMap::at`: constant-bpm path `llround(beats * sr*60/bpm)`; piecewise path accumulating each full
segment with `llround`, matching §Tempo map in project-model.md) at the **project's** sample rate —
48000 in every current project. Values are **unclamped**: a clip may extend past `length_bars` and its
true mapped position is reported (`end_sample` may exceed `total_samples_for(p)`; that is not an error).

**Omitted, never invented:** the clip trim keys `offset_beats` / `length_beats` (WP-0193a) are NOT part of
this view and do not affect its geometry (the renderer ignores them in placement); any other key present
in a project's clip object is dropped by the model parse and can never appear in the output.

Example — one named pattern (`"Intro"`, notes spanning 8 beats), clip at bar 1.5, repeats 2, gain −6.5,
fades 0.5/1.0 (120 bpm, 4/4, 48 kHz → spb 24000):

```json
[
  {
    "end_beats": 22.0,
    "end_sample": 528000,
    "fade_in_beats": 0.5,
    "fade_out_beats": 1.0,
    "gain_db": -6.5,
    "index": 0,
    "pattern": 0,
    "pattern_name": "Intro",
    "repeats": 2,
    "start_bar": 1.5,
    "start_beats": 6.0,
    "start_sample": 144000
  }
]
```

`opd clips` adds no operation and changes no model field; it is a pure view (the future playlist panel
and the copilot read this instead of parsing project JSON).

## Channel rack / mixer track view — `opd tracks` — additive (WP-0227)

```
opd tracks <proj.opj>
```

Read-only, deterministic: loads the project and prints **one JSON entry per track in model order**
(the order of the `tracks` array — never sorted by name or kind). It writes nothing, modifies no
project, and creates no files. Exactly one positional argument, no options; errors print nothing on
stdout: missing/extra argument or an option-looking token → exit 2 with detail
`tracks needs exactly <project.opj>`; file missing → exit 3 (`not_found`, detail = the path); project
fails to parse → exit 4 (`parse`, detail = the loader's message). A project whose `tracks` array is
empty never reaches this view: the loader rejects it first (exit 4, `tracks must be a non-empty
array`).

Engine API (`engine/tracks.hpp`, namespace `op`): `std::string tracks_json(const Project& p);` — a
pure function returning exactly the text `opd tracks` prints: a JSON array dumped with nlohmann
`dump(2)` (two-space indent; object keys sort alphabetically) plus **exactly one trailing `\n`**. A
`Project` built in memory with no tracks yields `[]\n`. Numbers are serialized by nlohmann exactly as
stored — never hand-format them: `index`, `choke_group` and the integer voice fields are JSON
integers; `gain_db`, `pan`, `attack_ms`, `release_ms` and `freq_hz` are float32 model values widened
to double, so their printed decimal is the shortest round-trip of the stored float (a project file
holding `"pan": 0.4` prints `0.4000000059604645` here — exactly what `op::project_bytes` writes for
the same track); ADSR/filter values are doubles at shortest round-trip.

Each entry has exactly these keys and no others:

| key | value |
|---|---|
| `index` | the track's position in `tracks` (0-based integer) |
| `name` | stored `track.name` (string, JSON-escaped by nlohmann) |
| `kind` | the voice kind string the project file writes: `"sampler"`, `"synth"`, `"metronome"` or `"sine"` |
| `gain_db` | stored `track.gain_db`; ALWAYS present (the default prints `0.0`) |
| `pan` | stored `track.pan`; ALWAYS present (the default prints `0.0`) |
| `mute` / `solo` | stored booleans; ALWAYS present — `false` prints as `false` (unlike the project file, which omits them at default) |
| `audible` | the mixer audibility computed by `op::track_audible(p, i)` — the renderer's own rule (§mixer in project-model.md), never a re-implementation: with any track soloed only non-muted solo tracks are audible; mute always wins over solo |
| `choke_group` | stored `track.choke_group`; ALWAYS present (`0` = none) |
| `voice` | the voice object exactly as the canonical project serializer writes it for this track — see below |

**The `voice` object (normative):** identical to `tracks[i]["voice"]` in the canonical serialization
of the same project (`op::project_bytes`) — produced by the file format's own voice serializer, so
**optional keys the model does not hold are ABSENT, never invented**:

- sampler: always `kind`, `path`; `start_s` only when non-zero (WP-0156), `end_s` only when a slice
  end is set, `root_note` only when a root note is set (WP-0167).
- synth: always `kind`, `attack_ms`, `release_ms`; `waveform` (`"sine"`/`"saw"`/`"square"`) only when
  the voice is not legacy; `adsr` — with ALL four inner keys `attack_s`, `decay_s`, `sustain`,
  `release_s` (defaults `0.005`, `0.0`, `1.0`, `0.030`) — only when an ADSR was given; `cutoff_hz`
  only when non-zero; `resonance` only when different from its default `0.7071` (WP-0161 canonical
  save).
- metronome: `kind`, `accent_every`. sine: `kind`, `freq_hz`.

Example — three tracks (`"Zeta Kick"` soloed sampler with trim, root note and choke group 2;
`"Alpha Lead"` muted synth v1 saw with ADSR and filter; `"Click"` metronome at defaults):

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

`opd tracks` adds no operation and changes no model field; it is a pure view (the future
channel-rack / mixer panel and the copilot read this instead of parsing project JSON).

## Crash-recovery inspection and restore — `opd recover` — additive (WP-0244)

```
opd recover <proj.opj> [--list | --restore -o <out.opj>]
```

`<proj>.autosave` (WP-0157, `op::Document::autosave_path`) holds the last autosaved work of a session that
may have died. This command reports that state and restores it with no GUI. It adds no model field, no
operation and changes no render: it only reads `<proj>` and `<proj>.autosave`, and writes the file named by
`-o`. It is a thin front end over the document layer — `op::Document::recovery_state`,
`op::Document::autosave_path`, `op::Document::open_recovered` and `Document::save_as` decide everything; no
recovery rule is re-implemented and `src/doc/` is not modified.

### States (decided in this order, exactly four)

1. **recoverable** — `op::Document::recovery_state(proj) == op::RecoveryState::AutosaveNewer`: both files
   load and their canonical `op::project_bytes` differ (newer content, i.e. work not in the saved file).
2. **missing** — not recoverable and `<proj>.autosave` does not exist.
3. **corrupt** — not recoverable, the file exists but `op::load_project` fails on it.
4. **identical** — not recoverable, the file exists and loads, so its content equals the saved file's.

`<proj>.autosave.tmp` (the temp file of an in-flight atomic autosave) is never considered: the document
layer never reads it, so neither does this command.

### Report — `--list`, and the default when neither flag is given

Exactly one line on stdout, and exit **0 in all four states**: the report never fails over the recovery
state, because "there is nothing to recover" is an answer, not an error.

```
recover: <proj>: autosave differs from the saved project (recoverable)
recover: <proj>: no autosave file
recover: <proj>: autosave matches the saved project
recover: <proj>: autosave unreadable (corrupt)
```

### Restore — `--restore -o <out.opj>`

The autosave is loaded with `op::Document::open_recovered` and written with `Document::save_as(out)`, which
is the canonical serializer (`op::save_project`) — so the output is a plain canonical `.opj`, and the
document layer's own save rules apply to it.

- Success: exit **0**, one stdout line `recover: <proj>: restored <proj>.autosave -> <out.opj>`. The bytes
  of `<out.opj>` equal the bytes of `<proj>.autosave`; `<proj>` and `<proj>.autosave` are left untouched.
- **missing** or **identical**: exit **3**, `error: not_found: no recoverable autosave for '<proj>'`, and
  nothing is written.
- **corrupt**: exit **4**, `error: parse: corrupt autosave '<proj>.autosave'`, and nothing is written.
- The project file itself is written **only** when `-o` names that very path: then `<proj>` gets the
  autosave content and `save_as` clears that path's autosave file (a stale `<out>.autosave` of a different
  output path is cleared the same way). That is the document layer's rule — the command must not delete any
  autosave file of its own accord.

### Errors (exit codes per §CLI: 2 usage, 3 not-found, 4 parse, 5 write error)

Exit **2** (usage) for: `opd recover` with no `<proj>`, any unknown argument, `--list` given together with
`--restore`, `--restore` without `-o`, and `-o` without `--restore`. Exit **3** when `<proj>` does not
exist. Exit **4** when `<proj>` exists but does not parse. A failure to write `<out.opj>` is exit **5**;
the project file and its autosave stay unchanged. `opd recover` never deletes, moves or truncates `<proj>`
except in the explicit `-o <proj>` case above, and it never runs a timer, autosave or scheduler.
