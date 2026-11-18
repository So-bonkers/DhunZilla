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
