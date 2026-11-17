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
