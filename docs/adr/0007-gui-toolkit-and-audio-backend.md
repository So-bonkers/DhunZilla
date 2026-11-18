# ADR-0007: JUCE 9 for the GUI shell + live audio; PipeWire via ALSA as the default Linux path

**Status:** Proposed · 2026-11-18 · technical decision taken autonomously (reversible);
**licensing consequence (§ Licensing) requires product-owner approval** because it amends the
practical effect of ADR-0001.

Evidence: `spikes/juce-probe/` (WP-0132) — source, `evidence/window.png`, `evidence/latency-sweep-pipewire.txt`,
`evidence/probe-default-window.txt`.

## Decision
1. **GUI toolkit + audio device layer: JUCE 9.0.5**, pinned via CMake `FetchContent` (tag tarball).
   It goes into a **new `src/app/` module only**. `src/engine/` and `src/cli/` stay JUCE-free so the
   deterministic core, `opd`, and the golden harness keep zero framework dependencies (ADR-0002 holds).
2. **Default Linux audio path: JUCE's ALSA backend talking to PipeWire's ALSA plugin**
   (device "PipeWire Sound Server"), 48 kHz, **128-sample default buffer**, user-selectable
   64–1024. This is shared (non-exclusive), so the DAW coexists with browser/system audio.
3. **JACK backend (`JUCE_JACK=1`) is a follow-up,** enabled once `pipewire-jack` is available on the
   rig (today only jackd2's libjack is installed and no JACK server runs). Direct `hw:` ALSA devices
   remain selectable for users who want exclusive access.
4. **All UI string literals go through UTF-8** (`juce::String (juce::CharPointer_UTF8 ("…"))` or
   `juce::String::fromUTF8`). A plain `const char*` is treated as Latin-1 by JUCE — the spike rendered
   "—" as mojibake until fixed. App code review must reject raw non-ASCII literals.

## Evidence (DGX Spark reference rig, aarch64, Ubuntu 24.04, PipeWire 48 kHz, 2026-11-18)
- Window renders under X11 (`DISPLAY=:1`); component snapshot captured (`evidence/window.png`).
- Device enumeration: 1 device type (ALSA), 24 outputs (PipeWire, NVIDIA HDMI ×4, HyperX USB headset).
- Untouched default: "Default ALSA Output (PipeWire)" @ 44.1 kHz / 512 → reported output latency **34.8 ms**.
  So the default must be set explicitly, not inherited.
- Explicit PipeWire @ 48 kHz, silent callbacks for 3 s each (callback interval measured with a
  preallocated, lock-free timestamp buffer):

  | buffer | period | reported output latency | measured mean / worst interval |
  |---:|---:|---:|---:|
  | 1024 | 21.33 ms | 64.00 ms | 21.22 / 22.32 ms |
  | 512 | 10.67 ms | 32.00 ms | 10.67 / 10.93 ms |
  | 256 | 5.33 ms | 16.00 ms | 5.35 / 5.73 ms |
  | **128** | **2.67 ms** | **8.00 ms** | **2.67 / 2.98 ms** |
  | 64 | 1.33 ms | 4.00 ms | 1.33 / 1.84 ms |
  | 32 | 0.67 ms | 2.00 ms | 0.67 / 1.87 ms |

  Reported output latency = 3 periods. The plan's "<10 ms" target is met for output at ≤128 samples.

### Limits of this evidence (honest)
- Callbacks did **no DSP**; xrun behavior under a real graph load is unmeasured (Phase 1 must add an
  xrun counter + load test before claiming a default).
- **True round-trip latency is unmeasured** — needs a physical loopback (line out → line in); the
  rig's only input is a wireless headset mic. Method, for when hardware exists: play a single-sample
  impulse every 500 ms on output ch1, record input ch1, cross-correlate, report median and p99 offset
  in samples over 60 s, at each buffer size; subtract nothing (that *is* the round trip). Expected at
  128 samples: ≈ 2×8 ms + converter delay.

## Licensing (needs owner sign-off)
JUCE 9 modules are dual-licensed **AGPLv3 / commercial JUCE licence**. Our core is GPLv3 (ADR-0001).
GPLv3 §13 explicitly permits combining with AGPLv3 code, so this is compatible, but:
- The **distributed GUI app** (anything linking `src/app/`) is effectively subject to AGPLv3 terms for
  the combined work. For a desktop DAW the extra AGPL obligation (offering source to users interacting
  over a network) is near-irrelevant — unless we ever host the app as a service (e.g. a cloud-render tier).
- The headless engine, `opd`, and the sidecar stay pure GPLv3 (no JUCE linkage).
- The plan already intends to bundle Vital (AGPLv3), so AGPL in the shipped bundle is not new in kind.
- A commercial JUCE licence would only matter if we ever wanted a non-copyleft build — not planned.

**Recommendation:** accept. **If rejected,** the fallback is Qt 6 (LGPLv3) + miniaudio/RtAudio +
a hand-rolled plugin host — roughly +2–3 months of Phase 1/2 work, mostly on plugin hosting.

## Alternatives considered
| Option | Why not (now) |
|---|---|
| Qt 6 / QML + RtAudio or miniaudio | Good UI tooling, LGPL; but no plugin hosting — VST3/CLAP host + editor embedding would be ours to build. |
| Dear ImGui + miniaudio | Fast to prototype, but immediate-mode UI fights accessibility, text input, and docking at DAW scale; no plugin hosting. |
| Tauri/web UI + native engine | Plan path C; IPC between UI and engine, plugin editor embedding impossible. Keep as a possible companion app. |
| Stay headless longer | Phase 1 exit criterion ("arrange a 3-min beat") needs a GUI. |

## Consequences
- New module boundary `src/app/` (JUCE): GUI + device I/O + graph executor that *reads* the project
  model and applies serializable model operations (AGENTS.md rule). No engine logic in `src/app/`.
- CI: app target built on both Linux arches; GUI smoke = headless component snapshot (no window
  manager needed — `createComponentSnapshot`), not a pixel golden.
- JUCE build adds ~2–4 min cold to CI; cache `_deps`.

## Revisit triggers
- Owner rejects AGPL consequence → switch to the Qt fallback before any `src/app/` code lands.
- xrun rate at 128 samples under a 16-track load test > 1/minute on the reference rig → raise default to 256.
- `pipewire-jack` becomes available → evaluate JACK backend latency vs ALSA-PipeWire; pick lower jitter.
- A macOS/Windows port is scheduled (ADR-0006) → CoreAudio/WASAPI come for free with JUCE; re-measure.
