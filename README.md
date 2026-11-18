# DhunZilla

**dhoon-ZIL-uh** — *dhun* (धुन) is the tune stuck in your head; *zilla* is the monster that makes it.

**The open-source, AI-native DAW.** FL Studio's fast pattern workflow where stems, MIDI and
natural language are first-class citizens — plus generation that lands in your session as an
*editable project*, not a flat file.

> Status: **Phase 0 (harness-first)** — building the deterministic render core + AI sidecar
> before any GUI. Provenance: plan at [`../ai-daw-plan.md`](../ai-daw-plan.md). Formerly
> **OpenPattern** (renamed 2026-11-18).
>
> Platforms: **Linux x86_64 + aarch64 only for now** ([ADR-0006](docs/adr/0006-linux-only-target.md)).
> macOS and Windows are deferred — the core stays portable so porting is a build/CI job later.

## Why

Incumbent DAWs bolt AI on top. DhunZilla designs the project format and workflow around AI
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

# AI sidecar (uv-managed; 72 tests incl. conformance suite over every backend)
cd ai-sidecar && uv sync && uv run pytest -q

# Optional: real Demucs stem separation (torch stays outside the sidecar core)
uv pip install --python /path/to/ml-venv demucs numpy   # one-time
OP_DEMUCS_PYTHON=/path/to/ml-venv/bin/python uv run pytest -q   # +7 tests incl. e2e
```

## CLI reference (`build/opd`)

| Command | What it does |
|---|---|
| `opd render <proj.opj> -o out.wav [--seed N] [--stems-dir DIR]` | Deterministic WAV render; optional per-track stem WAVs |
| `opd gen-testproject <out.opj>` | Generate a demo project + `kits/kick.wav` (quickstart fodder) |
| `opd midi2opj <in.mid> -o out.opj` | Import a Standard MIDI file as a new editable project |
| `opd import-midi <proj.opj> <in.mid> [-o out]` | Merge MIDI into an existing project as new tracks (grid semantics) |
| `opd attach <proj.opj> --path P --kind stem\|transcription\|other [--producer S]` | Reference an AI artifact with provenance (in-place unless `-o`) |
| `opd opj2mid <proj.opj> -o out.mid` | Export project to SMF format 1 @480 ppq (tempo map → FF51 events) |
| `opd pack <proj.opj> -o song.opd` | One shareable file: project + samples + existing attachments (valid zip) |
| `opd unpack <song.opd> -d DIR` | Verified extract — CRC-checked, traversal-guarded, all-or-nothing |
| `opd info <file>` | Summary for `.wav` / `.opj` / `.opd` / `.mid` (tracks, notes, tempo map, attachments…) |

Workflow glue lives in `harness/`: `make_earcheck.sh` (human review pack), `stem_into_project.sh`
(AI stems → project, with provenance), `midi_into_project.sh` (hum → editable tracks),
`roundtrip_eval.sh` (measured transcription fidelity), `sidecar_smoke.sh`, `run_goldens.sh`.

## The AI loop, end to end (headless today)

Render a synth melody → transcribe with Basic Pitch (ONNX) → import the MIDI back as an editable
project → re-render. Measured, not vibes:

```bash
OP_TRANSCRIBE_PYTHON=/path/to/onnx-venv/bin/python ./harness/roundtrip_eval.sh
# want=[(0.0, 60), (0.25, 64), (0.5, 67), (0.75, 64), (1.0, 60), (1.5, 67)]
# got =[(0.011, 60), (0.243, 64), (0.5, 67), (0.743, 64), (0.975, 60), (1.498, 67)]
# PITCH F1=1.00 (thr 0.8) PASS
# EVENT F1=1.00 (tol 0.08s, thr 0.8) PASS
```

Separate any audio into stems and wire them back into the project **with provenance** — then ship
it all as one file (`mocksep` backend runs this CI-safe with zero ML deps):

```bash
./harness/stem_into_project.sh my.song.opj some.wav     # or OP_DEMUCS_PYTHON=... for real demucs
# attached 4 stem(s) from demucs@htdemucs-demucs4.1.0 to my.song.opj
./build/opd pack my.song.opj -o my.song.opd             # song + kit + AI stems, one shareable file
```

Same idea for audio you *hummed*: transcribe → merge notes into the project as editable tracks →
attach the MIDI with provenance (`mocktrans` keeps this CI-safe; `OP_TRANSCRIBE_PYTHON=...` uses
real Basic Pitch):

```bash
./harness/midi_into_project.sh my.song.opj humming.wav
# transcribed via basicpitch@basic-pitch-nmp-onnx-ort1.32.0: notes merged into my.song.opj, MIDI attached at ai/transcribed.mid
```

And the flagship, end to end — **a text prompt becomes an editable project**, not a dead audio blob
(prompt → generate → transcribe → `.opj` + provenance attachments + render; byte-deterministic):

```bash
./harness/prompt_to_song.sh "rainy lofi drums with warm bass" ./my_song
# prompt -> editable song: ./my_song
#   song.opj (edit it, re-render, bundle it) · final_render.wav · ai/{source_audio.wav,transcribed.mid}
```

Setup for the transcription runtime lives in
[`ai-sidecar/backends/basicpitch/impl.py`](ai-sidecar/backends/basicpitch/impl.py) (why ONNX and
not TensorFlow: [ADR-0005](docs/adr/0005-transcription-onnx-runtime.md)). Stem splitting with
Demucs follows the same opt-in cartridge pattern (`OP_DEMUCS_PYTHON`).

## Ground rules

- Read [`AGENTS.md`](AGENTS.md) before contributing. All code is agent-written; humans are
  product owners + QA-with-ears.
- Every requirement ships as a spec + machine-checkable test, or it doesn't ship.
- Model weights are never committed — see [`docs/spec/model-manifest.md`](docs/spec/model-manifest.md).

## License

GPLv3 (see [LICENSE](LICENSE)). Vendored third-party under their own licenses
([third_party/](third_party/)); AI model weights carry their own licenses and are downloaded
separately per manifest.
