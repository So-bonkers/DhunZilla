# WP-0132 — Phase 0 close-out: GUI toolkit + audio path spike (JUCE)

**Status:** done (ADR-0007 proposed; licensing point awaits owner) · **Created:** 2026-11-18 · **Owner:** orchestrator
**Why now:** Phase 0 is engine/sidecar-only. The DAW the plan describes (Phase 1) needs a GUI and a
real-time audio device path; both are ADR-gated (ADR-0002 deferred JUCE). This WP produces the
evidence for that ADR — and the first pixel of DhunZilla.

## Scope
- Standalone spike (not linked into `src/`): minimal JUCE 9.0.5 GUI app on Linux/aarch64.
- Render a window and capture a screenshot as an artifact.
- Enumerate audio device types/names via `AudioDeviceManager`; report sample rate, buffer size and
  driver-reported input/output latency.
- Document the measurement method for true round-trip latency (loopback), since the dev container
  has no sound card.
- Draft ADR-0007 (GUI toolkit + audio backend) from the evidence.

## Out of scope
- Product UI, engine integration, plugin hosting, mixer widgets.
- Changing the core build or adding JUCE to `src/` (ADR-gated; spike lives in `spikes/`).
- Golden/fixtures.

## Environment facts (dev rig, 2026-11-18)
- aarch64, 20 cores, Ubuntu 24.04; `DISPLAY=:1` (X11) available.
- ALSA 1.2.11 + JACK 1.9.21 headers present. ~~no sound card~~ **Corrected 2026-11-18 16:40:** after the
  reboot the rig exposes PipeWire (48 kHz, quantum 1024, min 32), NVIDIA HDMI ×4 and a HyperX USB headset.
  libjack is jackd2's (no `pipewire-jack`), no JACK server running.
- cmake 3.31.6, g++ 13.3.

## Acceptance criteria
- [x] Spike builds from a clean tree with pinned JUCE (`FetchContent`, tag 9.0.5).
- [x] Window renders; PNG screenshot committed or attached as evidence.
- [x] Device probe output captured (device types, names, or explicit "none").
- [x] Latency measurement method documented; reported-latency numbers captured when a device exists.
- [x] ADR-0007 drafted with the decision + alternatives + revisit triggers.

## Handoff report
- **What changed:** `spikes/juce-probe/` (JUCE 9.0.5 via FetchContent; not linked into `src/`): window +
  device enumeration + configurable device/sr/buffer (env `DZ_DEVICE/DZ_SR/DZ_BUFFER`), 3 s silent-callback
  interval measurement (preallocated, lock-free), `DZ_NO_WINDOW=1` probe-only mode, UTF-8 string fix.
  `docs/adr/0007-gui-toolkit-and-audio-backend.md` (Proposed).
- **How verified:** `cmake -S spikes/juce-probe -B spikes/juce-probe/build -DCMAKE_BUILD_TYPE=Release && cmake --build ... -j6`
  → built (only warnings are inside JUCE). Sweep (PipeWire @48 kHz): 1024→64.0 ms, 512→32.0, 256→16.0,
  **128→8.0 ms reported output latency**, 64→4.0, 32→2.0; callback interval mean matches period, worst
  ≤ +0.5 ms except 32-sample (1.87 ms worst vs 0.67 period). Untouched default (44.1k/512) = 34.8 ms.
- **Demo artifact:** `spikes/juce-probe/evidence/window.png`, `evidence/latency-sweep-pipewire.txt`,
  `evidence/probe-default-window.txt`.
- **Follow-ups noticed (not done):** (1) `src/app/` skeleton per ADR-0007 once approved; (2) xrun counter +
  16-track load test at 128/256; (3) `JUCE_JACK=1` once `pipewire-jack` is installed (needs sudo: owner);
  (4) true round-trip measurement needs a loopback cable — method in ADR-0007; (5) CI job building
  the spike/app on both arches with `_deps` cache.
- **PROPOSE:** add the UTF-8 literal rule (ADR-0007 §4) to AGENTS.md "Code conventions" when `src/app/` lands.
- **BLOCKED:** none. Owner decision pending: ADR-0007 licensing consequence (AGPLv3 via JUCE 9).
- Executed by: Claude (elephant) — continued from the earlier orchestrator session's build.
