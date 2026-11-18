# WP-0114 — `opd import-midi` (merge MIDI into an existing project)

**Spec:** docs/spec/project-model.md §MIDI merge · **Status: done** · Owner: orchestrator

## Scope
Engine `import_midi_into(Project&, midi_path, err)` + CLI `opd import-midi <proj.opj> <in.mid> [-o]`.
Append each note-bearing MIDI track as a new synth-voice op track; grid semantics (beats map 1:1).
Out of scope (untouched): tempo rescale mode, arrangement auto-clipping, stem audio merge.

## Acceptance — evidence
- `merge.ok` / `merge.tracks_appended`: host 1→3 tracks, patterns appended in order.
- `merge.indices_sequential`: new pattern `.track` fields = 1,2 (caught + fixed a real off-by-index:
  pre-push track count *is* the index; my first cut did `size()+i` and produced 1,3 — probe confirmed).
- `merge.grid_semantics_exact`: merged note positions/pitches identical to standalone `load_midi`.
- `merge.length_not_shrunk` (8-bar host unchanged) + `merge.length_extends` (0.25→1 bar to fit).
- `merge.missing_fails` + `merge.failure_leaves_project_untouched`: temp-first build, no partial state.
- `merge.render_ok` / `render_has_audio`: merged project renders with sound.
- CLI e2e: gen-testproject 3 tracks → import two_notes.mid → **5 tracks**, render OK (768 KB).
- op_tests **141 assertions ALL PASS**; goldens 6 fixtures unchanged; build warning-free.

## Handoff report
**What changed:** new engine function + CLI command + spec section. No existing behavior altered —
goldens prove it (bit-exact, untouched). Model-driven mutation via load→op→save per AGENTS.md.

**How verified:** 10 unit assertions on the merge op (structure, indices, grid exactness, length
rules, failure atomicity, render), plus a CLI round-trip on a generated project. One real bug found
and fixed by the index assertion; documented in the acceptance line so a future reader knows why the
comment "pre-push track count == this track's index" is load-bearing.

**Metrics:** +12 assertions (129→141). No new dependencies. No interface files beyond midi.hpp/.cpp,
main.cpp, test_main.cpp (5 files — within the AGENTS.md boundary).

**Follow-ups noticed (PROPOSE, not done here):**
- `--rescale` mode: map MIDI tempo→project tempo so imported content keeps *duration*, not grid
  position. Needs a decision on which wins when they conflict; file as its own WP.
- Import could optionally auto-add arrangement clips for the new tracks when the host has an
  arrangement — currently we only warn. UX call; keep engine pure either way.
- MIDI merge + `.opd`: a bundle packed after import already carries everything (verified pattern),
  so no special handling needed — noting it so nobody re-investigates.
