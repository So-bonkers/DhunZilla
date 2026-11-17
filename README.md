# OpenPattern

**The open-source, AI-native DAW.** FL Studio's fast pattern workflow where stems, MIDI and
natural language are first-class citizens — plus generation that lands in your session as an
*editable project*, not a flat file.

> Status: **Phase 0 (harness-first)** — building the deterministic render core + AI sidecar
> before any GUI. Provenance: plan at [`../ai-daw-plan.md`](../ai-daw-plan.md). Codename is
> provisional; name selection is an open human decision (see ROADMAP).

## Why

Incumbent DAWs bolt AI on top. OpenPattern designs the project format and workflow around AI
artifacts — separated stems, transcriptions, symbolic scores, embeddings — from day one.
Suno gives you a finished MP3; we hand you a session you keep producing in.

## What's here now (Phase 0)

| Area | Path | State |
|---|---|---|
| Deterministic render core (headless C++20) | `src/` | building — metronome/sampler voices → WAV |
| Golden-file test harness | `harness/` | bit-exact + dBFS diff tooling |
| AI sidecar (Python, adapter-based) | `ai-sidecar/` | adapter SDK, mock+real backends, conformance suite |
| Specs & decisions | `docs/spec/`, `docs/adr/` | authoritative — agents implement these |
| Work packages | `TASKS/` | the unit of all execution |

## Quickstart (dev)

```bash
# Core: build + tests + golden render (deterministic, no GUI needed)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/opd gen-testproject demo.json
./build/opd render demo.json -o out.wav --seed 42

# AI sidecar (uv-managed)
cd ai-sidecar && uv sync && uv run pytest -q
```

## Ground rules

- Read [`AGENTS.md`](AGENTS.md) before contributing. All code is agent-written; humans are
  product owners + QA-with-ears.
- Every requirement ships as a spec + machine-checkable test, or it doesn't ship.
- Model weights are never committed — see [`docs/spec/model-manifest.md`](docs/spec/model-manifest.md).

## License

GPLv3 (see [LICENSE](LICENSE)). Vendored third-party under their own licenses
([third_party/](third_party/)); AI model weights carry their own licenses and are downloaded
separately per manifest.
