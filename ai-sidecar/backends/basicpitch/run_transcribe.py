#!/usr/bin/env python3
"""Interpreter-side transcription wrapper for the basicpitch cartridge (WP-0008).

Runs inside OP_TRANSCRIBE_PYTHON (a venv with basic-pitch --no-deps + onnxruntime;
TF is deliberately absent — see docs/adr/0005-transcription-onnx-runtime.md).

Usage: run_transcribe.py INPUT_WAV OUT_MIDI [MIN_HZ MAX_HZ]

basic_pitch resolves its bundled model by runtime availability (TF → CoreML →
TFLite → ONNX); with only onnxruntime installed it lands on the shipped nmp.onnx.
Exit codes: 0 ok · 2 usage/input error · 3 inference failure.
"""

from __future__ import annotations

import sys


def main() -> int:
    if len(sys.argv) not in (3, 5):
        print("usage: run_transcribe.py INPUT_WAV OUT_MIDI [MIN_HZ MAX_HZ]", file=sys.stderr)
        return 2
    inp, outp = sys.argv[1], sys.argv[2]
    kwargs: dict[str, float] = {}
    if len(sys.argv) == 5:
        try:
            kwargs["minimum_note_frequency"] = float(sys.argv[3])
            kwargs["maximum_note_frequency"] = float(sys.argv[4])
        except ValueError:
            print("min/max Hz must be numbers", file=sys.stderr)
            return 2

    from basic_pitch import ICASSP_2022_MODEL_PATH
    from basic_pitch.inference import Model, predict

    try:
        model = Model(ICASSP_2022_MODEL_PATH)
        _, midi_data, note_events = predict(inp, model, **kwargs)
        midi_data.write(outp)
    except Exception as exc:  # noqa: BLE001 — boundary: report, don't traceback-spam the parent
        print(f"transcription failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 3
    print(f"notes={len(note_events)} midi_written={outp}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
