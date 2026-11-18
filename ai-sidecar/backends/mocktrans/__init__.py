"""mocktrans backend package — deterministic canned MIDI for CI (see impl.py)."""

from .impl import MockTransBackend

BACKEND_CLASS = MockTransBackend

__all__ = ["BACKEND_CLASS", "MockTransBackend"]
