# Attachments — informational AI-artifact references (WP-0116, 2026-11-18)

`"attachments": [ {"path", "kind", "producer"?} ]` in the project model. Files that live beside
the project but are **not part of the render graph**: separated stems, transcriptions, reference
renders. The renderer never reads them; they exist so tools (and humans) can see what AI produced
for a song and carry it along when sharing.

- `path`: project-relative, `/`-separated, no `..` segments, not absolute — same safety rules as
  bundle entries. `kind`: `stem` | `transcription` | `other`. `producer`: free-form provenance
  string (tool + version/job id), omitted when empty in canonical output.
- **Engine op:** `add_attachment(Project&, Attachment, err)` validates and appends; rejects
  duplicates by path; leaves the project untouched on failure. Model-driven per AGENTS.md — CLI
  (`opd attach`) is a thin caller. The engine never invents attachments (no wall-clock anywhere).
- **`.opd` packing:** existing attachment files are bundled alongside sampler assets (deduped,
  sorted → deterministic); missing ones are skipped silently — informational refs may legitimately
  point at external libraries, unlike samplers whose absence fails the pack.
- Canonical save omits the whole array when empty → legacy projects byte-identical.
