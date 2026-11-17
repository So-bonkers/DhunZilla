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
| WP-0001 | Repo scaffold: layout, LICENSE, AGENTS.md, specs, ADRs, CI config | in-progress | orchestrator |
| WP-0002 | Deterministic render core v0 (metronome/sine/sampler → PCM16 WAV) + unit tests | in-progress | orchestrator |
| WP-0003 | Golden-file harness: golden_diff.py + run_goldens.sh + fixtures | pending | orchestrator |
| WP-0004 | Sidecar v0: FastAPI job queue, adapter base, mock backends, conformance suite green | in-progress | subagent-sidecar |
| WP-0005 | Model manager: manifest schema (license tri-state), downloader w/ sha256 + gates, tests | pending | subagent-sidecar |
| WP-0006 | Real adapter: Demucs (CPU path) behind manifest gate; one end-to-end separation demo | pending | orchestrator |
| WP-0007 | CI workflow: six-target matrix config (win x64/arm64, mac universal, linux x64/arm64) | pending | orchestrator |
| WP-0008 | Basic-Pitch transcription adapter (ONNX or python pkg) + MIDI F1 conformance hook | queued | — |

## Open human decisions (blocking nothing; flagged for review)
1. **Name** — "OpenPattern" provisional; trademark/domain check required before public launch.
2. Signing certs budget (~$100–500/yr): Apple dev account + Azure Trusted Signing/EV.
3. Approve ADR-0002 (headless-core-first, JUCE deferred) and ADR-0003 (HTTP sidecar v0).

## Nightly (once repo is pushed)
Six-target CI; reference-rig (DGX Spark aarch64) CPU-inference evals; determinism sweep across arches.
