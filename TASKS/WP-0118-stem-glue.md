# WP-0118 — stem→project glue with auto-provenance

**Status: done** · Owner: orchestrator · Spec surface: existing docs/spec/attachments.md + sidecar HTTP API (no schema changes)

## Scope
`harness/stem_into_project.sh <proj.opj> [audio.wav]`: starts the sidecar, runs a separate job
(backend = $OP_STEM_BACKEND; demucs when its runtime is configured, else mocksep for CI), copies
each artifact to `<proj-dir>/stems/<name>.wav`, attaches it via `opd attach` with producer
`<backend>@<model_version>`. Zero new engine/sidecar code — pure composition of shipped parts.

## Acceptance — evidence
- MODE A (mocksep, zero deps): `attached 4 stem(s) from mocksep@mocksep-0.1.0`; info lists all
  four; works headless in CI conditions.
- MODE B (real demucs CPU): `attached 4 stem(s) from demucs@htdemucs-demucs4.1.0` on a rendered
  project — provenance carries the exact model tag.
- Full loop: `opd pack` of the augmented project → .opd entries = song.opj + kit + **4 stems**
  (2.8 MB): one shareable file containing human song + AI artifacts + who made them.
- Gate convention honored: demucs requested without OP_DEMUCS_PYTHON → exit 3 SKIP.

## Handoff report
**What changed:** one harness script + docs. Module boundaries respected: the Python half talks to
the sidecar over its HTTP API and to the C++ core only through the documented CLI (`opd attach`) —
no format parsing across the boundary (AGENTS.md).
**Follow-ups noticed (PROPOSE):** transcription variant of this glue (`hum.wav → import-midi +
attach .mid`) is a near-copy; fold into one `harness/ai_into_project.sh` if a third variant ever
appears. Also: stems are copies — dedup by hash inside .opd later if sizes matter.
