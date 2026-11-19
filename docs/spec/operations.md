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
