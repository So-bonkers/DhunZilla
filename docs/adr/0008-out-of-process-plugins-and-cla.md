# ADR-0008: Out-of-process plugin protocol ("everything is a plugin") + CLA before outside contributions

**Status:** Proposed · 2026-11-20 · awaiting product-owner approval. Context: the owner deferred the monetisation
decision until the prototype has real users (plan §6a) and asked that no option be closed off in the meantime.
These two choices are what keep every option open; everything else about monetisation stays undecided.

## Decision
1. **Extensions run out of process.** DhunZilla's extension model is "everything is a plugin" (inspired by the
   DeepSeek Harness design): AI features, the future copilot, MCP access and third-party tools are separate
   processes that talk to the app over a **public, versioned protocol** — JSON-RPC over stdio or local HTTP,
   the same arm's-length style the AI sidecar already uses (ADR-0003). The protocol exposes read access to the
   project, the action catalog (plan, Phase 2) and **proposals of operation journals** (WP-0135 ops) that the user
   confirms; every applied change goes through `op::History` (undoable) and can be previewed with `opd diff`.
   No plugin code is loaded into the app or engine address space.
2. **Contributor License Agreement before any outside contribution.** Until a CLA is in place, the project does
   not accept external code contributions (issues and discussion are welcome). Contributions from the owner and
   the project's own agents are unaffected.

## Why
- **Licensing.** The engine/CLI/sidecar are GPLv3 and the GUI app is AGPLv3 via JUCE 9 (ADR-0001, ADR-0007). Code
  loaded into the same process and sharing data structures is generally treated as one combined work, so an
  in-process plugin would have to be (A)GPL-compatible. A separate process speaking a public protocol is a separate
  work: first-party plugins can stay open, and a plugin *may* be licensed differently (including paid) if the owner
  later chooses that — without touching the core's licence.
- **Copyright consolidation.** Offering any future dual or commercial licence requires holding the rights to the
  code. Without a CLA every outside contributor co-owns their part and that option disappears.
- **Engineering, independent of licensing.** Process isolation means a crashing or misbehaving plugin cannot take
  down the audio engine (AGENTS.md: the real-time thread is a dumb executor), plugins can be written in any
  language, and the same protocol serves MCP clients, the copilot and our own end-to-end test drivers.

## Consequences
- The plugin protocol is designed once (Phase 2, with the action catalog) and versioned; MCP v0 (Phase 3) and the
  copilot (Phase 5) are clients of it. No WP is pulled forward by this ADR.
- Real-time audio plugins (CLAP/VST3 hosting, Phase 2) are a separate concern: they follow their own host model and
  sandboxing plan and are not covered here.
- External contributors see a clear notice (CONTRIBUTING.md) until the CLA exists.
- Open question for the owner, at launch: an IP-lawyer check on the copyright status of agent-written code, since it
  affects licence enforcement and any future licence sales.
