# WP-0112 — `opd info` for .opd bundles

**Spec:** docs/spec/bundle.md (listing = metadata-only CD read, names validated, no CRC pass) · **Status: done** · Owner: orchestrator

## Evidence
- `bundle.list_entries` unit test (order + names); CLI output verified on packed e2e artifact.
- op_tests 129 assertions ALL PASS; goldens unchanged.

## Follow-ups noticed
- Could extend listing with sizes/CRCs when a UI file browser needs it — trivial from same CD walk.
