# Spec: Model Manifest (weights registry) — `ai-sidecar/models/manifest.json`

Weights are never committed. The manifest is the only way a model enters the system. Entries are
fetched from HF (or other URLs) on demand, hash-pinned, and license-gated.

```jsonc
{
  "schema": 1,
  "models": [{
    "id": "htdemucs",                       // stable key used by backends
    "backend": "demucs",                    // adapter id that consumes it
    "task": "stem_separation",
    "sources": [{"url": "...", "sha256": "...", "size_bytes": 123}],   // hash-pinned, resumable downloads
    "cache_dir": "models/cache/htdemucs",
    "license": {
      "code": "MIT",                        // adapter code / upstream repo code license
      "weights_tier": "personal-use-ok",    // TRI-STATE: "permissive" | "personal-use-ok" | "commercial-needs-contact"
      "weights_spdx": "NOASSERTION",        // or e.g. "CC-BY-NC-4.0", "Apache-2.0"
      "notes": "Demucs weights: research-derived (MUSDB); upstream license unclear — verified 2026-11-18",
      "verified_on": "2026-11-18"
    },
    "default_enabled": false,               // true ONLY for permissive tier
    "status": "live",                       // "live" | "awaiting-weights" (pre-wired stubs)
    "capabilities_hint": {"stems": ["vocals","drums","bass","other"], "vram_tiers": {"cpu":"slow-ok","6gb":"ok","24gb":"ok"}}
  }]
}
```

## Rules
- `license_tier` semantics: **permissive** (Apache/MIT/BSD/CC0) → default_enabled allowed;
  **personal-use-ok** (individuals incl. monetized outputs, e.g. YuE2 pattern) → shipped in registry,
  default-off, explicit accept dialog required before download; **commercial-needs-contact** → registry-visible, never auto-download, docs link to licensor.
- `awaiting-weights` stubs pre-wire unreleased models (e.g. next ACE-Step): capability claims recorded from announcements with source URL + date; flipping to `live` must re-trigger license verification.
- sha256 mismatch → hard fail, quarantine file, no retry-loop.
- The model manager never sends more than the URL host sees (no project data in requests).

## Offline verify and pin — additive (WP-0173)
Two offline operations on `ModelManager` (no network, no license gate — they only read/hash local files),
exposed as `opsidecar models verify|pin`. They are how a placeholder `sha256` (`hash_verified: false`)
becomes a real pin: put the weights in the cache dir by any trusted route, then `pin`.

### `ModelManager.verify(model_id) -> dict`
Hashes each source's file `<cache_dir>/<basename of url>` (never the network). Result:
`{"id", "ok", "files": [{"file", "status", "expected_sha256", "actual_sha256"}, ...]}`, files in source
order. `status`: `missing` (file absent; `actual_sha256` null), `size_mismatch` (source declares
`size_bytes` and the file differs; not hashed, `actual_sha256` null), `hash_mismatch`, `ok`.
`ok` is true only if there is at least one file and every file is `ok`. An `awaiting-weights` entry raises
`ModelAwaitingWeights` ("nothing to verify"); an unknown id raises `ModelNotFound`.

### `ModelManager.pin(model_id, *, from_dir=None) -> dict`
For each source, measures `<from_dir or cache_dir>/<basename of url>`; if every file exists it rewrites
`manifest.json` atomically (temp file + rename, `json.dumps(indent=2)` + newline) setting each source's
`sha256` and `size_bytes` to the measured values and `hash_verified` to `true`. Nothing else in the
file changes. If any file is missing it raises `WeightsMissing` (error code `weights_missing`) and the
manifest is untouched. `awaiting-weights` entries raise `ModelAwaitingWeights`. Result:
`{"id", "pinned": [{"file", "sha256", "size_bytes"}, ...]}`.

### CLI
`opsidecar models verify <id> [--manifest PATH]` prints the verify dict as JSON (indent 2, sorted keys),
exit 0 if `ok` else 1. `opsidecar models pin <id> [--manifest PATH] [--from-dir DIR]` prints the pin dict,
exit 0. Any `ModelManagerError` prints `error: <error_code>: <message>` to stderr, nothing to stdout,
exit 1. `--manifest` defaults to the bundled/`OPSIDECAR_MANIFEST` manifest.
