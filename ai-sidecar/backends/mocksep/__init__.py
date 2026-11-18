"""mocksep backend package — 4-band FFT stem splitter (see impl.py)."""

from .impl import MockSepBackend

BACKEND_CLASS = MockSepBackend

__all__ = ["BACKEND_CLASS", "MockSepBackend"]
