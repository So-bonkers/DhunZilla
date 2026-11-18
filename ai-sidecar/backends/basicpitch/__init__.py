"""basicpitch backend package — audio→MIDI via ONNX runtime (see impl.py)."""

from .impl import BasicPitchBackend

BACKEND_CLASS = BasicPitchBackend

__all__ = ["BACKEND_CLASS", "BasicPitchBackend"]
