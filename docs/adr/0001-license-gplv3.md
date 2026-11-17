# ADR-0001: Core licensed GPLv3; SDK boundary permissive later

**Status:** Proposed (awaiting human approval) · 2026-11-18

## Decision
OpenPattern core is GPLv3. Vendored deps under compatible licenses only, recorded in `third_party/`
with license files (currently nlohmann/json MIT). Plugin SDK / extension API will be published
under a permissive license (BSD/MIT) when introduced (Phase 2), keeping the SDK≠core boundary.

## Context
Matches JUCE's GPL option, Surge XT, Vital, aubio/Chordino; makes Essentia usable without dual-license
fees. AI model weights are handled separately via manifest + tri-state license gating (spec/model-manifest.md)
— they are never linked into the core and never committed.

## Consequences
- Commercial DAW vendors can't close-source derivatives — acceptable for an FOSS-first project.
- VST2 support explicitly out of scope (Steinberg EULA/trademark). VST3 hosting via SDK is a Phase 2 ADR.
