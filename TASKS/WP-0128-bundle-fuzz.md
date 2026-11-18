# WP-0128 — .opd bundle structural fuzzing

**Status: done** · Owner: orchestrator · Completes the untrusted-input matrix (MIDI/JSON/WAV×2/bundle).

## Scope
Hostile central directories against unpack_bundle + list_bundle: crafted cases (huge nlen, cd_off
past EOF, inflated entry count, data corruption), tail-heavy truncation sweep, 400 seeded mutations.
Acceptance: bool-or-clean-error always; ASAN clean.

## Findings — evidence
- **Heap-buffer-overflow (READ of size 65535)** found by the crafted nlen case under ASAN before fix:
  central entry name was constructed as std::string(ptr, nlen) without checking name bytes exist
  in-file; both readers affected. Fix = one bounds check per parser (`p + 46 + nlen > size` → corrupt-entry).
- Also hardened while inside: **duplicate entry names** (previously silent last-wins overwrite) now
  rejected explicitly.
- Post-fix: ASAN+UBSAN clean over the whole fuzz corpus, op_tests **ALL PASS (~210 assertions)**,
  goldens 6/6 bit-exact, CLI pack→unpack e2e smoke OK.

## Handoff report
**What changed:** bundle.cpp +2 bounds checks +duplicate-reject (both parsers); test_main.cpp +bf suite;
ROADMAP. Zero format change — legitimate bundles pack/unpack byte-identically (goldens/e2e prove it).
**Remaining known-and-accepted v0 semantics:** entries can reference the same underlying bytes only
via dedup at pack time (already done); EOCD comment tolerance means trailing garbage after EOCD is
ignored by scan (standard zip behavior); no encryption/zip64 — format is ours, store-only.
