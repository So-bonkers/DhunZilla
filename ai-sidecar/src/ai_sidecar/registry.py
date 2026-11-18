"""Backend discovery: import adapter packages from the ``backends/`` directory.

Discovery never trusts the adapters — a backend that raises on import, forgets
``BACKEND_CLASS``, or lies about its id is reported with an actionable error and the
rest of the sidecar keeps working (spec: "absence must never break the sidecar").

Usage example::

    from ai_sidecar.registry import discover_backends
    backends = discover_backends()          # {"mockgen": <MockGenBackend>, ...}
    backend = backends["mockgen"]
"""

from __future__ import annotations

import hashlib
import importlib.util
import os
import sys
from pathlib import Path
from types import ModuleType

from ai_sidecar.base import BackendBase

BACKENDS_DIR_ENV = "OPSIDECAR_BACKENDS_DIR"
"""Set this env var to point discovery at an alternative backends/ directory (tests)."""


class RegistryError(RuntimeError):
    """Raised when the backends directory itself is unusable (not per-backend breakage)."""


def default_backends_dir() -> Path:
    """Locate the ``backends/`` package dir.

    Order: ``OPSIDECAR_BACKENDS_DIR`` env var, else ``<project>/backends`` — resolved
    relative to this file, which yields ``ai-sidecar/backends`` in dev/editable installs
    and ``site-packages/backends`` for a wheel install (both are ``parents[2]/backends``).
    """
    env = os.environ.get(BACKENDS_DIR_ENV)
    if env:
        return Path(env).expanduser().resolve()
    return Path(__file__).resolve().parents[2] / "backends"


def _load_parent_package(backends_dir: Path) -> ModuleType:
    """Load ``backends/`` as a synthetic package so adapter subpackages can use
    relative imports. The module name is salted with the directory path to avoid
    stale sys.modules entries when tests point discovery at different directories."""
    if not backends_dir.is_dir():
        raise RegistryError(
            f"backends directory not found: {backends_dir} "
            f"(set {BACKENDS_DIR_ENV} or create it)"
        )
    init = backends_dir / "__init__.py"
    if not init.is_file():
        raise RegistryError(f"{backends_dir} is not a package (missing __init__.py)")
    salt = hashlib.sha1(str(backends_dir).encode("utf-8")).hexdigest()[:12]
    name = f"opsidecar_backends_{salt}"
    cached = sys.modules.get(name)
    if cached is not None:
        return cached
    spec = importlib.util.spec_from_file_location(
        name, init, submodule_search_locations=[str(backends_dir)]
    )
    if spec is None or spec.loader is None:
        raise RegistryError(f"cannot build an import spec for {init}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    try:
        spec.loader.exec_module(module)
    except Exception as exc:  # noqa: BLE001 - report, don't crash the sidecar
        del sys.modules[name]
        raise RegistryError(f"backends/__init__.py failed to import: {exc!r}") from exc
    return module


def discover_backends(backends_dir: Path | None = None) -> dict[str, BackendBase]:
    """Import every adapter subpackage and return ``{backend_id: instance}``.

    Each subpackage must expose ``BACKEND_CLASS`` (a concrete BackendBase subclass).
    Broken adapters are skipped with a warning — the CI floor is that the mocks load.
    """
    directory = backends_dir if backends_dir is not None else default_backends_dir()
    parent = _load_parent_package(directory)
    found: dict[str, BackendBase] = {}
    problems: list[str] = []

    for sub in sorted(p for p in directory.iterdir() if (p / "__init__.py").is_file()):
        mod_name = f"{parent.__name__}.{sub.name}"
        try:
            module = importlib.import_module(mod_name)
        except Exception as exc:  # noqa: BLE001 - a broken adapter must not break us
            problems.append(f"{sub.name}: import failed: {exc!r}")
            continue
        cls = getattr(module, "BACKEND_CLASS", None)
        if cls is None or not (isinstance(cls, type) and issubclass(cls, BackendBase)):
            problems.append(
                f"{sub.name}: missing BACKEND_CLASS (a BackendBase subclass); skipping"
            )
            continue
        try:
            instance = cls()
        except Exception as exc:  # noqa: BLE001
            problems.append(f"{sub.name}: instantiation failed: {exc!r}")
            continue
        if not instance.id:
            problems.append(f"{sub.name}: backend id must be non-empty; skipping")
            continue
        if instance.id in found:
            problems.append(
                f"{sub.name}: duplicate backend id '{instance.id}' "
                f"(already provided by another adapter); skipping"
            )
            continue
        found[instance.id] = instance

    if problems:
        import warnings

        for problem in problems:
            warnings.warn(f"backend discovery: {problem}", RuntimeWarning, stacklevel=2)
    return found
