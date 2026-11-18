# ADR-0003: AI sidecar v0 over HTTP/JSON (FastAPI); gRPC deferred

**Status:** Accepted (human approval — product owner) · 2026-11-18

## Decision
v0 sidecar transport is FastAPI + JSON over localhost with SSE progress. The interface surface
(jobs, backends, models) is defined so a gRPC or unix-socket transport can replace it without
touching adapters (spec/sidecar-protocol.md).

## Context
The plan names gRPC/ZMQ; for Phase 0 the priority is an adapter/conformance floor that runs
everywhere with minimal toolchain friction (protobuf codegen across six targets would slow the
harness-first goal). HTTP gives us: trivial curl-based acceptance tests, SSE progress, and a
transport shape identical to any future cloud rendering service.

## Consequences
- + speed of iteration; − no typed contracts until buf/protos land (mitigated by pydantic schemas as single source).
- Performance-sensitive bulk transfer is file-path based anyway (artifacts on disk), so serialization overhead is negligible.
