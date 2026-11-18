"""CLI alias package so ``python -m opsidecar`` works (WP-0004 naming).

The real implementation lives in :mod:`ai_sidecar` — the import name fixed by
docs/spec/backend-adapters.md. This shim only forwards to it; nothing else should
import this package.
"""

from ai_sidecar.cli import main

__all__ = ["main"]
