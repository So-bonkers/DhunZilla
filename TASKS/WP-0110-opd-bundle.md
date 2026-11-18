# WP-0110 — `.opd` project bundle

**Spec:** docs/spec/bundle.md · **Status: done** · Owner: orchestrator

## Scope
pack_project/unpack_bundle + `opd pack|unpack`; store-only ZIP, no new dependencies.

## Acceptance — evidence
- Byte-deterministic double-pack (`bundle.pack_byte_deterministic`).
- Round-trip: unpack → load → render **bit-identical** to original (sampler path resolves from
  bundle-restored kits/) — unit + CLI e2e both prove it.
- Guards: CRC corruption rejected; hand-built `../evil.txt` zip refused (path traversal); missing
  asset fails pack with actionable message.
- Interop: Info-ZIP `unzip -t` reports "No errors detected" on our output.
- **First full ASAN+UBSAN sweep of the entire test suite: zero findings, ALL PASS** (114 assertions).

## Bugs found & fixed en route (both real, both by tests)
1. `read_file_bytes` never set its out-flag true → pack always failed after writing nothing.
2. EOCD central-directory-size included 12 already-written EOCD bytes → `unzip` rejected the
   archive ("12 bytes too long"); now computed before appending.

## Follow-ups noticed
- Store-only = bigger files; compression needs vendored zlib + ADR (noted in spec).
- Bundles don't yet carry sidecar job outputs (stems/MIDI) — natural when project-file
  references to AI artifacts land (Phase 1 model work).
