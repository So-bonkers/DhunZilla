# WP-0116 — AI-artifact attachments

**Spec:** docs/spec/attachments.md · **Status: done** · Owner: orchestrator

## Scope
`Attachment{path,kind,producer?}` in the model + `add_attachment` engine op + `opd attach` CLI +
`.opd` packing of existing attachment files. Out of scope: render-graph integration (stems *as*
tracks already works via sampler voice), UI affordances.

## Acceptance — evidence
- Parse 2 attachments (producer default empty); 4-way rejection matrix: absolute, `..`, bad kind,
  empty path (`att.rejects_bad_input` ×4).
- Op behavior: add ok / duplicate rejected / bad kind rejected; round-trip preserves all fields.
- Bundle: existing stem packed into .opd (asserted via list_bundle + CLI unzip grep), missing ref
  skipped without failing pack (`att.bundle_carries_existing`, `att.bundle_skips_missing`).
- Legacy byte-stability: attachments omitted when empty — goldens 6/6 unchanged.
- CLI e2e: attach → info shows `[stem] stems/drums.wav <- demucs ht-demo` → pack contains it.
- op_tests **166 assertions ALL PASS**; ASAN+UBSAN full sweep clean after the model.cpp surgery.

## Handoff report
**What changed:** model.hpp/cpp (struct + parse + save + op), bundle.cpp (attachment collection),
main.cpp (`attach` command, info listing), tests. One mid-package self-inflicted wound documented
honestly: a function-definition edit consumed the `voice_to_json` signature line (the same anchor
trap flagged in earlier handoffs) and briefly duplicated two functions; repaired by verified
line-range surgery with assertions on boundary lines — lesson re-applied: never end an insertion
edit's new_string without re-including the anchor text it matched.

**Metrics:** +17 assertions (149→166). Zero new deps. 4 code files touched (within boundaries).

**Follow-ups noticed (PROPOSE):**
- Sidecar could emit ready-made `opd attach` lines in job results so glue scripts do provenance
  automatically (one line per produced stem/midi).
- `kind` may want `sample-pack` later; enum growth is additive, old files unaffected.
