#!/usr/bin/env python3
"""Tests for tools/render_bench.py (WP-0175).

Run with::

    python3 -m unittest discover tools
"""

from __future__ import annotations

import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import render_bench  # noqa: E402

# A fake opd: writes a float32 WAV to the -o path. FAKE_OPD_MODE switches it
# to nondeterministic (random data), failing (non-zero exit) or junk-output.
FAKE_OPD = """#!{python}
import os, struct, sys, time

def find_out(argv):
    return argv[argv.index("-o") + 1]

mode = os.environ.get("FAKE_OPD_MODE", "ok")
if mode == "fail":
    sys.stderr.write("fake opd exploded\\n")
    sys.exit(7)

out = find_out(sys.argv)
rate, channels, block_align = 48000, 2, 8
frames = 100
fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * block_align,
                  block_align, 32)
if mode == "junk":
    with open(out, "wb") as f:
        f.write(b"this is not a wav file at all")
    sys.exit(0)

samples = [0.25] * (frames * channels)
if mode == "nondeterministic":
    # Varies per subprocess: pid plus a nanosecond clock reading.
    samples[0] = float(os.getpid()) + (time.time_ns() % 1000000) / 1e6
data = struct.pack("<" + "f" * len(samples), *samples)
chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt \\
       + b"data" + struct.pack("<I", len(data)) + data
with open(out, "wb") as f:
    f.write(b"RIFF" + struct.pack("<I", 4 + len(chunks)) + b"WAVE" + chunks)
""".format(python=sys.executable)


def make_wav(frames=100, channels=2, rate=48000, bits=32, float_fmt=True,
             extra_chunk=None):
    """Build minimal RIFF/WAVE bytes for wav_info tests."""
    block_align = channels * (bits // 8)
    data_len = frames * block_align
    fmt = struct.pack("<HHIIHH", 3 if float_fmt else 1, channels, rate,
                      rate * block_align, block_align, bits)
    chunks = [(b"fmt ", fmt)]
    if extra_chunk is not None:
        chunks.append((b"LIST", extra_chunk))
    chunks.append((b"data", bytes(data_len)))
    body = b""
    for i, (cid, cbody) in enumerate(chunks):
        body += cid + struct.pack("<I", len(cbody)) + cbody
        if len(cbody) & 1 and i != len(chunks) - 1:
            body += b"\0"  # pad byte after odd-size chunk
    return b"RIFF" + struct.pack("<I", 4 + len(body)) + b"WAVE" + body


class RenderBenchTestBase(unittest.TestCase):
    """Gives each test a unique temp dir (includes the pid) and a fake opd."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp(prefix=f"wp0175_{os.getpid()}_")
        self.opd = os.path.join(self.tmp, "opd")
        with open(self.opd, "w", encoding="utf-8") as f:
            f.write(FAKE_OPD)
        os.chmod(self.opd, 0o755)
        self.project = os.path.join(self.tmp, "t.opj")
        with open(self.project, "w", encoding="utf-8") as f:
            f.write("{}\n")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def run_main(self, argv):
        """Run render_bench.main(argv); return (exit_code, stdout, stderr)."""
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = render_bench.main(argv)
        return code, out.getvalue(), err.getvalue()


class TestWavInfo(unittest.TestCase):
    def test_pcm_float(self) -> None:
        self.assertEqual(render_bench.wav_info(make_wav()), (48000, 2, 100))

    def test_pcm16(self) -> None:
        self.assertEqual(
            render_bench.wav_info(make_wav(frames=50, channels=1, rate=44100,
                                           bits=16, float_fmt=False)),
            (44100, 1, 50))

    def test_extra_chunk_before_data(self) -> None:
        # Odd-size LIST chunk: exercises chunk skipping and the pad byte.
        payload = b"INFOISYP" + b"x" * 5  # 13 bytes, odd
        self.assertEqual(render_bench.wav_info(make_wav(extra_chunk=payload)),
                         (48000, 2, 100))

    def test_junk_input_raises(self) -> None:
        for junk in (b"", b"not a wav", b"RIFFxxxxWAVE"):
            with self.assertRaises(ValueError):
                render_bench.wav_info(junk)

    def test_missing_data_chunk_raises(self) -> None:
        wav = make_wav()
        head = wav[:12] + wav[12:12 + 8 + 16]  # RIFF header + fmt only
        with self.assertRaises(ValueError):
            render_bench.wav_info(head)


class TestCli(RenderBenchTestBase):
    def test_report_keys_and_determinism(self) -> None:
        json_out = os.path.join(self.tmp, "report.json")
        code, out, err = self.run_main(
            ["--opd", self.opd, self.project, "--runs", "3", "--seed", "7",
             "--json", json_out])
        self.assertEqual(code, 0, err)
        report = json.loads(out)
        self.assertEqual(set(report), {
            "project", "runs", "seed", "sample_rate", "channels", "frames",
            "bytes", "audio_seconds", "wall_seconds", "min_s", "median_s",
            "max_s", "realtime_factor", "sha256", "distinct_outputs",
            "deterministic"})
        self.assertEqual(report["project"], self.project)
        self.assertEqual(report["runs"], 3)
        self.assertEqual(report["seed"], 7)
        self.assertEqual((report["sample_rate"], report["channels"],
                          report["frames"]), (48000, 2, 100))
        self.assertAlmostEqual(report["audio_seconds"], 100 / 48000)
        self.assertEqual(len(report["wall_seconds"]), 3)
        self.assertTrue(report["deterministic"])
        self.assertEqual(report["distinct_outputs"], 1)
        self.assertEqual(len(report["sha256"]), 64)
        # --json writes the same text as stdout (stdout ends with a newline).
        with open(json_out, encoding="utf-8") as f:
            self.assertEqual(f.read(), out)

    def test_nondeterministic_exit_1(self) -> None:
        os.environ["FAKE_OPD_MODE"] = "nondeterministic"
        self.addCleanup(os.environ.pop, "FAKE_OPD_MODE", None)
        code, out, err = self.run_main(
            ["--opd", self.opd, self.project, "--runs", "3"])
        self.assertEqual(code, 1)
        report = json.loads(out)
        self.assertFalse(report["deterministic"])
        self.assertGreaterEqual(report["distinct_outputs"], 2)
        self.assertIn("NONDETERMINISTIC: ", err)

    def test_opd_failure_exit_3(self) -> None:
        os.environ["FAKE_OPD_MODE"] = "fail"
        self.addCleanup(os.environ.pop, "FAKE_OPD_MODE", None)
        code, out, err = self.run_main(["--opd", self.opd, self.project])
        self.assertEqual(code, 3)
        self.assertIn("error: render failed (exit 7): fake opd exploded", err)

    def test_junk_output_exit_3(self) -> None:
        os.environ["FAKE_OPD_MODE"] = "junk"
        self.addCleanup(os.environ.pop, "FAKE_OPD_MODE", None)
        code, _, err = self.run_main(["--opd", self.opd, self.project])
        self.assertEqual(code, 3)
        self.assertTrue(err.startswith("error:"))

    def test_usage_errors_exit_2(self) -> None:
        missing = os.path.join(self.tmp, "nope.opj")
        code, _, err = self.run_main(["--opd", self.opd, missing])
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error:"))

        not_exec = os.path.join(self.tmp, "plain.txt")
        with open(not_exec, "w", encoding="utf-8") as f:
            f.write("hello\n")
        code, _, err = self.run_main(["--opd", not_exec, self.project])
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error:"))

        code, _, err = self.run_main(
            ["--opd", self.opd, self.project, "--runs", "0"])
        self.assertEqual(code, 2)
        self.assertTrue(err.startswith("error:"))

    def test_expect_sha256_match_and_mismatch(self) -> None:
        code, out, _ = self.run_main(["--opd", self.opd, self.project])
        self.assertEqual(code, 0)
        good = json.loads(out)["sha256"]
        code, _, err = self.run_main(
            ["--opd", self.opd, self.project, "--expect-sha256", good])
        self.assertEqual(code, 0)
        code, _, err = self.run_main(
            ["--opd", self.opd, self.project, "--expect-sha256", "ab" * 32])
        self.assertEqual(code, 1)
        self.assertIn("GOLDEN MISMATCH: expected", err)

    def test_min_realtime_exit_4(self) -> None:
        code, out, err = self.run_main(
            ["--opd", self.opd, self.project, "--min-realtime", "1e9"])
        self.assertEqual(code, 4)
        self.assertIn("TOO SLOW: realtime factor", err)
        # A tiny floor passes.
        code, _, _ = self.run_main(
            ["--opd", self.opd, self.project, "--min-realtime", "0.001"])
        self.assertEqual(code, 0)


if __name__ == "__main__":
    unittest.main()
