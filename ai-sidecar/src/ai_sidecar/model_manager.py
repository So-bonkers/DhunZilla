"""Model manager: manifest schema, sha256-pinned downloader, license tri-state gates.

Implements docs/spec/model-manifest.md (WP-0005). Weights never ship in git; the
manifest is the sole gate. Downloads use httpx with Range-resume, verify sha256 and
**quarantine on mismatch (no retry loop)**. License rules:

- ``permissive``            → download allowed without an accept flag.
- ``personal-use-ok``       → refused unless the caller passes ``accept=True``.
- ``commercial-needs-contact``→ never auto-downloaded, even with ``accept=True``.
- ``status: awaiting-weights`` stubs → refused with an actionable message pointing at
  the announcement source recorded in ``capabilities_hint``.

Usage example::

    from pathlib import Path
    from ai_sidecar.model_manager import ModelManager, LicenseNotAccepted

    mgr = ModelManager(Path("models/manifest.json"))
    print([m["id"] for m in mgr.view()])           # ids + availability for /models
    try:
        mgr.download("htdemucs")                    # personal-use-ok → refused
    except LicenseNotAccepted as exc:
        mgr.download("htdemucs", accept=True)       # explicit accept → allowed
"""

from __future__ import annotations

import hashlib
import os
from datetime import date  # noqa: TC003 - pydantic evaluates at runtime
from enum import StrEnum
from pathlib import Path
from typing import Annotated, Any
from urllib.parse import urlparse

import httpx
from pydantic import (
    BaseModel,
    ConfigDict,
    Field,
    ValidationError,
    field_validator,
    model_validator,
)

MANIFEST_ENV = "OPSIDECAR_MANIFEST"
"""Env var pointing at an alternative manifest.json (used by tests)."""

CHUNK_SIZE = 1 << 20  # 1 MiB download chunks


# --------------------------------------------------------------------------- errors


class ModelManagerError(Exception):
    """Base for model-manager failures; carries a protocol error code."""

    error_code = "model_manager_error"
    retryable = False


class ManifestError(ModelManagerError):
    """Manifest file missing/unparseable/schema-invalid."""

    error_code = "manifest_invalid"


class ModelNotFound(ModelManagerError):
    """No manifest entry with that id."""

    error_code = "unknown_model"


class LicenseNotAccepted(ModelManagerError):
    """License tier requires explicit acceptance the caller did not provide."""

    error_code = "license_not_accepted"


class ModelAwaitingWeights(ModelManagerError):
    """Pre-wired stub for an unreleased model — nothing to download yet."""

    error_code = "model_awaiting_weights"


class HashMismatch(ModelManagerError):
    """sha256 did not match the manifest pin; the file was quarantined, no retry."""

    error_code = "hash_mismatch"


class DownloadFailed(ModelManagerError):
    """Network/HTTP failure while fetching a source (retryable)."""

    error_code = "download_failed"
    retryable = True


# ----------------------------------------------------------------- manifest schema


class LicenseTier(StrEnum):
    """Tri-state weights license tier (docs/spec/model-manifest.md)."""

    PERMISSIVE = "permissive"
    PERSONAL_USE_OK = "personal-use-ok"
    COMMERCIAL_NEEDS_CONTACT = "commercial-needs-contact"


class ModelStatus(StrEnum):
    """Manifest entry lifecycle."""

    LIVE = "live"
    AWAITING_WEIGHTS = "awaiting-weights"


class Source(BaseModel):
    """One hash-pinned, resumable download location."""

    model_config = ConfigDict(extra="forbid")

    url: str
    sha256: Annotated[str, Field(pattern=r"^[0-9a-f]{64}$")]
    size_bytes: int | None = Field(default=None, gt=0)
    hash_verified: bool = False
    """False → the pinned sha256 is a formatted placeholder, NOT an upstream-verified
    digest (WP-0005 honesty rule). Downloads still verify against it."""

    @field_validator("url")
    @classmethod
    def _http_url(cls, v: str) -> str:
        scheme = urlparse(v).scheme
        if scheme not in ("http", "https"):
            raise ValueError(f"source url must be http(s), got scheme {scheme!r}")
        return v


class License(BaseModel):
    """License block per docs/spec/model-manifest.md — notes/verified_on mandatory."""

    model_config = ConfigDict(extra="forbid")

    code: str = Field(min_length=1, description="Adapter/upstream code license (SPDX-ish)")
    weights_tier: LicenseTier
    weights_spdx: str = Field(min_length=1)
    notes: str = Field(min_length=1)
    verified_on: date


class ModelEntry(BaseModel):
    """One model registry entry."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    id: str = Field(min_length=1)
    backend: str = Field(min_length=1)
    task: str = Field(min_length=1)
    sources: list[Source] = Field(default_factory=list)
    cache_dir: str = Field(min_length=1)
    license: License  # noqa: A003 - field name fixed by the manifest spec
    default_enabled: bool = False
    status: ModelStatus = ModelStatus.LIVE
    capabilities_hint: dict[str, Any] = Field(default_factory=dict)

    @model_validator(mode="after")
    def _consistency(self) -> ModelEntry:
        if self.status is ModelStatus.LIVE and not self.sources:
            raise ValueError(f"live entry '{self.id}' must declare at least one source")
        if self.default_enabled and self.license.weights_tier is not LicenseTier.PERMISSIVE:
            raise ValueError(
                f"entry '{self.id}': default_enabled=true only allowed for "
                "weights_tier=permissive"
            )
        if self.status is ModelStatus.AWAITING_WEIGHTS and self.sources:
            raise ValueError(
                f"awaiting-weights entry '{self.id}' must not declare sources yet"
            )
        return self


class Manifest(BaseModel):
    """Top-level manifest.json document."""

    model_config = ConfigDict(extra="forbid", populate_by_name=True)

    schema_version: Annotated[int, Field(alias="schema")]
    models: list[ModelEntry] = Field(min_length=1)

    @model_validator(mode="after")
    def _check(self) -> Manifest:
        if self.schema_version != 1:
            raise ValueError(f"unsupported manifest schema {self.schema_version}, expected 1")
        ids = [m.id for m in self.models]
        dupes = sorted({i for i in ids if ids.count(i) > 1})
        if dupes:
            raise ValueError(f"duplicate model ids: {dupes}")
        return self


def load_manifest(path: Path) -> Manifest:
    """Parse+validate manifest.json; every failure becomes an actionable ManifestError."""
    try:
        raw = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ManifestError(f"cannot read manifest {path}: {exc}") from exc
    try:
        return Manifest.model_validate_json(raw)
    except ValidationError as exc:
        first = "; ".join(
            f"{'.'.join(str(p) for p in err['loc'])}: {err['msg']}" for err in exc.errors()[:5]
        )
        raise ManifestError(f"manifest {path} failed schema validation: {first}") from exc


def default_manifest_path() -> Path:
    """Locate manifest.json: ``OPSIDECAR_MANIFEST`` env var, else ``<ai-sidecar>/models/``."""
    env = os.environ.get(MANIFEST_ENV)
    if env:
        return Path(env).expanduser().resolve()
    return Path(__file__).resolve().parents[2] / "models" / "manifest.json"


# ------------------------------------------------------------------ model manager


class ModelManager:
    """Manifest-backed registry + gated downloader."""

    def __init__(self, manifest_path: Path | None = None) -> None:
        self.manifest_path = manifest_path if manifest_path is not None else default_manifest_path()

    # -- registry views -----------------------------------------------------------

    def manifest(self) -> Manifest:
        """Freshly load the manifest (small file; no caching, always current)."""
        return load_manifest(self.manifest_path)

    def get_entry(self, model_id: str) -> ModelEntry:
        """Return one entry by id; raise ModelNotFound with the known ids listed."""
        for entry in self.manifest().models:
            if entry.id == model_id:
                return entry
        known = [m.id for m in self.manifest().models]
        raise ModelNotFound(f"no model '{model_id}' in manifest; known ids: {known}")

    def cache_root(self, entry: ModelEntry) -> Path:
        """Resolve an entry's cache_dir (relative paths resolve against the sidecar root,
        i.e. the parent of the directory holding the manifest)."""
        p = Path(entry.cache_dir)
        if p.is_absolute():
            return p
        return self.manifest_path.resolve().parent.parent / p

    def local_available(self, entry: ModelEntry) -> bool:
        """True when every source file is present in cache_dir with the declared size.

        Cheap check on purpose (size, not hash): hashing multi-GB weights on every
        GET /models would be silly; downloads always verify sha256 end-to-end.
        """
        root = self.cache_root(entry)
        if not entry.sources:
            return False  # no sources = nothing obtainable; vacuous truth would be wrong here
        for source in entry.sources:
            f = root / filename_for(source.url)
            if not f.is_file():
                return False
            if source.size_bytes is not None and f.stat().st_size != source.size_bytes:
                return False
        return True

    def view(self) -> list[dict[str, Any]]:
        """Rows for GET /models: tier + availability + status + gate info."""
        rows: list[dict[str, Any]] = []
        for entry in self.manifest().models:
            rows.append(
                {
                    "id": entry.id,
                    "backend": entry.backend,
                    "task": entry.task,
                    "status": entry.status.value,
                    "license_tier": entry.license.weights_tier.value,
                    "weights_spdx": entry.license.weights_spdx,
                    "license_notes": entry.license.notes,
                    "verified_on": entry.license.verified_on.isoformat(),
                    "default_enabled": entry.default_enabled,
                    "hash_verified": all(s.hash_verified for s in entry.sources),
                    "local_available": self.local_available(entry),
                    "requires_accept": entry.license.weights_tier is not LicenseTier.PERMISSIVE,
                }
            )
        return rows

    # -- gates ---------------------------------------------------------------------

    def check_gate(self, entry: ModelEntry, *, accept: bool) -> None:
        """Raise unless the manifest license rules allow a download right now."""
        if entry.status is ModelStatus.AWAITING_WEIGHTS:
            hint = entry.capabilities_hint or {}
            src = hint.get("announcement_url", "no source url recorded")
            when = hint.get("announced_on", "date unknown")
            raise ModelAwaitingWeights(
                f"model '{entry.id}' has status awaiting-weights: the weights are not "
                f"released, so there is nothing to download. Capability claims are "
                f"announcement-era and UNVERIFIED (source: {src}, announced: {when}). "
                "Track that URL; flipping the entry to 'live' re-triggers license "
                "verification before any download is allowed."
            )
        tier = entry.license.weights_tier
        if tier is LicenseTier.PERMISSIVE:
            return
        if tier is LicenseTier.COMMERCIAL_NEEDS_CONTACT:
            raise LicenseNotAccepted(
                f"model '{entry.id}' weights are tier 'commercial-needs-contact': "
                "never auto-downloaded. Contact the licensor (see license notes), then "
                "place the weights into the cache dir manually."
            )
        if not accept:
            raise LicenseNotAccepted(
                f"model '{entry.id}' weights are tier 'personal-use-ok' and require "
                'explicit acceptance: re-send POST /models/{}/download with body '
                '{{"accept": true}} after reviewing: {}'.format(entry.id, entry.license.notes)
            )

    # -- downloader ------------------------------------------------------------------

    def download(
        self,
        model_id: str,
        *,
        accept: bool = False,
        client: httpx.Client | None = None,
    ) -> dict[str, Any]:
        """License-gated fetch of every source; verify sha256, quarantine on mismatch.

        Returns ``{"id", "files": [...], "sha256_verified"}``. Pass ``client`` to reuse
        a connection (tests inject one); otherwise an httpx.Client is created per call.
        """
        entry = self.get_entry(model_id)
        self.check_gate(entry, accept=accept)
        root = self.cache_root(entry)
        root.mkdir(parents=True, exist_ok=True)

        files: list[str] = []
        owns_client = client is None
        active = client if client is not None else httpx.Client(timeout=httpx.Timeout(30.0))
        try:
            for source in entry.sources:
                dest = root / filename_for(source.url)
                self._fetch_to_file(active, source, dest)
                files.append(str(dest))
        finally:
            if owns_client:
                active.close()
        return {
            "id": model_id,
            "files": files,
            "sha256_verified": all(s.hash_verified for s in entry.sources),
        }

    def _fetch_to_file(self, client: httpx.Client, source: Source, dest: Path) -> None:
        """Single attempt (no retry loop): Range-resume into ``<dest>.part``, hash it,
        rename on match — move to ``<dest>.quarantined`` on mismatch."""
        if dest.is_file() and _size_ok(dest, source.size_bytes):
            return  # already present

        part = dest.with_name(dest.name + ".part")
        resume_from = part.stat().st_size if part.is_file() else 0
        headers = {"Range": f"bytes={resume_from}-"} if resume_from > 0 else {}
        try:
            with client.stream("GET", source.url, headers=headers) as resp:
                if resp.status_code == 206:
                    mode = "ab"
                elif resp.status_code == 200:
                    mode = "wb"  # server ignored the Range header — restart cleanly
                    resume_from = 0
                else:
                    raise DownloadFailed(
                        f"GET {source.url} returned HTTP {resp.status_code}; expected "
                        "200/206"
                    )
                digest = hashlib.sha256()
                if mode == "ab":
                    with part.open("rb") as existing:
                        for block in iter(lambda: existing.read(CHUNK_SIZE), b""):
                            digest.update(block)
                with part.open(mode) as out:
                    for chunk in resp.iter_bytes(chunk_size=CHUNK_SIZE):
                        out.write(chunk)
                        digest.update(chunk)
        except httpx.HTTPError as exc:
            raise DownloadFailed(f"GET {source.url} failed: {exc}") from exc

        got = digest.hexdigest()
        if got != source.sha256:
            quarantine = dest.with_name(dest.name + ".quarantined")
            os.replace(part, quarantine)
            raise HashMismatch(
                f"sha256 mismatch for {source.url}: manifest pins {source.sha256}, "
                f"downloaded {got}. The partial download was quarantined at {quarantine}; "
                "not retrying. Verify the manifest pin or fix the server."
            )
        os.replace(part, dest)


def filename_for(url: str) -> str:
    """Best-effort local filename for a source URL (basename of its path)."""
    name = Path(urlparse(url).path).name
    return name if name else "download.bin"


def _size_ok(path: Path, expected: int | None) -> bool:
    return expected is None or path.stat().st_size == expected
