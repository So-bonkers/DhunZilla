"""demucs backend package — real stem separation via configured interpreter (see impl.py)."""

from .impl import DemucsBackend

BACKEND_CLASS = DemucsBackend

__all__ = ["BACKEND_CLASS", "DemucsBackend"]
