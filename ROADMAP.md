# ROADMAP — OpenPattern

Source plan: `../ai-daw-plan.md` (workspace root). This file is the in-repo index; work packages live in `TASKS/`.

## Phases
| Phase | Theme | Status |
|---|---|---|
| 0 | Harness-first: repo, CI matrix, deterministic render core, golden harness, sidecar skeleton + adapters | **IN PROGRESS** |
| 1 | Skeleton DAW (transport, channel rack, playlist, mixer, sampler) | pending |
| 2 | Real DAW (piano roll, recording, automation, CLAP/VST3 hosting) | pending |
| 3 | AI hook (native stem-splitter, audio→MIDI, sing→instrument, sample search) | pending |
| 4 | Generation layer (adapter SDK live, ACE-Step/YuE2, beatbox→drums, mix assistant) | pending |
| 5 | Copilot & polish (scripting API + NL editing, realtime AI FX, 1.0) | pending |

## Work packages — Phase 0
| WP | Title | Status | Owner |
|---|---|---|---|
| WP-0001 | Repo scaffold | **done** | orchestrator |
| WP-0002 | Deterministic render core v0 | **done** | orchestrator |
| WP-0003 | Golden-file harness | **done** | orchestrator |
| WP-0004 | Sidecar v0 (FastAPI + adapters + conformance) | **done** (59 tests) | subagent+orchestrator |
| WP-0005 | Model manager (manifest, sha256, license gates) | **done** | subagent+orchestrator |
| WP-0006 | Demucs CPU cartridge (opt-in) + e2e separation through /jobs | **done** (aarch64 CPU) | orchestrator |
| WP-0007 | CI workflow: six-target matrix config | **done** (untested until push) | orchestrator |
| WP-0008 | Basic-Pitch transcription adapter (ONNX or python pkg) + MIDI F1 conformance hook | queued | — |
| WP-0103a | Synth voice (MIDI-pitched, 12-TET) + note pitch field — closes hum→MIDI→sound engine path | **done** | orchestrator |
| WP-0103b | `opd midi2opj`: SMF reader → project model (import loop with basicpitch) | next | orchestrator |
| WP-0101 | Arrangement layer: clips {pattern, start_bar, repeats}, legacy-compatible, bit-exact | **done** | orchestrator |
| WP-0102 | Basic-Pitch transcription cartridge via ONNX runtime (ADR-0005; real MIDI e2e on aarch64) | **done** | orchestrator |
| WP-0010 | Native stem export (`--stems-dir`) — solo stems recombine bit-exact to mix | **done** | orchestrator |

## Open human decisions (blocking nothing; flagged for review)
1. **Name** — "OpenPattern" provisional; trademark/domain check required before public launch.
2. Signing certs budget (~$100–500/yr): Apple dev account + Azure Trusted Signing/EV.
3. Approve ADR-0002 (headless-core-first, JUCE deferred) and ADR-0003 (HTTP sidecar v0).

## Nightly (once repo is pushed)
Six-target CI; reference-rig (DGX Spark aarch64) CPU-inference evals; determinism sweep across arches.
