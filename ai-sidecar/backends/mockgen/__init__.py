"""mockgen backend package — deterministic sine-chord generator (see impl.py)."""

from .impl import MockGenBackend

BACKEND_CLASS = MockGenBackend

__all__ = ["BACKEND_CLASS", "MockGenBackend"]
