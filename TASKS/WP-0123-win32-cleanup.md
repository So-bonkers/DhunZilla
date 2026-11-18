# WP-0123 — dead _WIN32 block removal (ADR-0006 hygiene)

**Status: done** · Owner: orchestrator

Removed the two `#ifdef _WIN32` drive-letter checks (model.cpp valid_attachment, bundle.cpp
safe_entry_name) — unreachable on supported platforms per ADR-0006. Portable checks unchanged
(leading /, backslash, .. segments). Evidence: 182 assertions ALL PASS incl. att.rejects_bad_input;
goldens 6/6; zero behavior change on Linux (blocks were compiled out before too — pure deletion).
