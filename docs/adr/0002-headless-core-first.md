# ADR-0002: Headless deterministic core first; JUCE integration deferred and gated

**Status:** Accepted (autonomous, reversible) · 2026-11-18

## Decision
Phase 0 builds the render engine as a **GUI-free C++20 library + CLI (`opd`)** with zero framework
dependencies (vendored single-header only). JUCE (audio drivers, GUI, plugin hosting) is integrated
later via its own ADR once the deterministic core + golden harness prove out.

## Context
The source plan recommends JUCE for the product shell, but the harness-first strategy needs the
opposite property: a minimal, bit-exact, cross-arch render path that runs headless in CI and agent
sessions (this dev box is a containerized aarch64 with no display). Building the core without a
framework keeps determinism auditable (`-ffp-contract=off`, libm-only via dsp.hpp wrappers) and makes
every future engine bug reproducible by a CLI command.

## Consequences
- Golden-render tests work from day 1 on all six CI targets, no audio hardware required.
- JUCE integration becomes a well-scoped adapter layer later (graph executor over our model), not
  an architectural assumption. Risk: some rework of the driver/glue layer — accepted; the model and
  render semantics carry forward unchanged.
