# ADR-0006: Linux-only target for now (x86_64 + aarch64); macOS/Windows deferred

**Status:** Accepted (product owner) · 2026-11-18

## Decision
For the current phase (Phase 0 close-out through the first MVP), **Linux is the only supported
platform**, on both **x86_64 and aarch64**. That means:

- CI builds and tests only Linux (x86_64 + aarch64). No MSVC, no Apple toolchain, no Windows
  runner labels.
- Docs, install instructions and issue templates assume Linux.
- The engine code stays **portable by discipline**: no Linux-only syscalls in `src/engine/`,
  no compiler intrinsics, no platform conditionals in the render path. A later macOS/Windows
  port should be a build/CI job, not a rewrite.
- Bit-exactness is still asserted across **both Linux arches** — the cross-arch determinism
  guard is preserved, just narrowed to the platforms we actually test.

This explicitly descops plan §6b's six-target matrix and amends ADR-0002's "all six CI
targets". Those targets are **deferred, not abandoned**.

## Context
The plan called for six targets green from day 1. In practice no Windows or macOS machine has
been available, CI has not run yet, and GUI/real-time work (where platform differences bite
hardest) has not started. Maintaining six configs and runner labels nobody can verify produces
false confidence and slows every work package. The dev rig (DGX Spark, aarch64 Linux) is the
only machine that can produce real signal today, and Linux x86_64 covers the common CI case.
Cutting the matrix now keeps the harness honest: every target we claim is one we actually test.

## Consequences
- Faster CI, fewer unverifiable claims; agents no longer write blind Windows/macOS code paths.
- Public messaging must say "Linux only" — no implied cross-platform support.
- Risk: platform-specific bugs surface later (audio backend, plugin hosting, file dialogs).
  Mitigation: keep the engine/CLI platform-agnostic (already true) and revisit **before the
  first public beta**, when a GUI exists and porting decisions can be made with evidence.

## Revisit trigger
Revisit when any of these is true: first GUI shell lands, first public beta is scheduled, or a
contributor with macOS/Windows hardware joins. Porting then starts with CI + a build check, not
with a rewrite.
