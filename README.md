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
# Core: build + tests + golden renders (deterministic, no GUI needed)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/op_tests                        # 43 assertions
./harness/run_goldens.sh                # bit-exact re-render + golden match + negative control
./harness/test_cli_errors.sh            # CLI exit-code contracts

# Render, with native stem export:
./build/opd gen-testproject demo.json
./build/opd render demo.json -o out.wav --seed 42 --stems-dir stems/

# AI sidecar (uv-managed; 59 tests + conformance suite over all backends)
cd ai-sidecar && uv sync && uv run pytest -q

# Optional: real Demucs stem separation (torch stays outside the sidecar core)
uv pip install --python /path/to/ml-venv demucs numpy   # one-time
OP_DEMUCS_PYTHON=/path/to/ml-venv/bin/python uv run pytest -q   # +7 tests incl. e2e
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
