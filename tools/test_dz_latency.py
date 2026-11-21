#!/usr/bin/env python3
"""Tests for tools/dz_latency.cpp (WP-0190): the per-block callback budget report for dz::Player.

Run with the three env vars set (otherwise everything is skipped)::

    DZ_LATENCY_BIN=build/dz_latency DZ_OPD_BIN=build/opd \\
    DZ_PROJECT=tests/fixtures/goldens/arranged.opj python3 -m unittest -v tools/test_dz_latency.py
"""

from __future__ import annotations

import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

REQUIRED_ENV = ("DZ_LATENCY_BIN", "DZ_OPD_BIN", "DZ_PROJECT")

# Byte-exact key order of the report (tools/dz_latency.md).
EXPECTED_KEYS = [
    "project",
    "sample_rate",
    "block_frames",
    "blocks",
    "total_frames",
    "budget_ms",
    "max_ms",
    "p99_ms",
    "p50_ms",
    "overruns",
    "output_identical",
    "reference_match",
]


@unittest.skipUnless(
    all(os.environ.get(k) for k in REQUIRED_ENV),
    "set DZ_LATENCY_BIN, DZ_OPD_BIN and DZ_PROJECT to run the dz_latency tests",
)
class DzLatencyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.bin = os.environ["DZ_LATENCY_BIN"]
        cls.opd = os.environ["DZ_OPD_BIN"]
        cls.project = os.environ["DZ_PROJECT"]
        cls.tmpdir = tempfile.mkdtemp(prefix=f"dz_latency_{os.getpid()}_")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmpdir, ignore_errors=True)

    # ---- helpers ----

    def run_tool(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [self.bin, *args], capture_output=True, text=True, check=False
        )

    def render_ref(self, name: str) -> str:
        """`opd render PROJECT -o <tmp>/<name>`; returns the output path."""
        out = os.path.join(self.tmpdir, name)
        proc = subprocess.run(
            [self.opd, "render", self.project, "-o", out],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, f"opd render failed: {proc.stderr}")
        return out

    def report(self, *args: str) -> dict:
        proc = self.run_tool(*args)
        self.assertEqual(proc.returncode, 0, f"exit {proc.returncode}: {proc.stderr}")
        return json.loads(proc.stdout)

    # ---- tests ----

    def test_json_key_order_and_blocks_math(self):
        rep = self.report(self.project)
        self.assertEqual(list(rep.keys()), EXPECTED_KEYS)
        self.assertEqual(rep["block_frames"], 128)
        self.assertEqual(
            rep["blocks"], math.ceil(rep["total_frames"] / 128), "blocks == ceil(total_frames/128)"
        )
        self.assertEqual(rep["sample_rate"], 48000)
        self.assertEqual(rep["project"], self.project)
        # Default budget: block * 1000 / sample_rate, printed with %.6f.
        self.assertIn('"budget_ms": 2.666667,', self.run_tool(self.project).stdout)
        self.assertTrue(rep["output_identical"])

    def test_reference_match_true_exit_0(self):
        ref = self.render_ref("ref.wav")
        proc = self.run_tool(self.project, "--reference", ref)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        rep = json.loads(proc.stdout)
        self.assertTrue(rep["output_identical"])
        self.assertIs(rep["reference_match"], True)

    def test_reference_match_null_without_option(self):
        proc = self.run_tool(self.project)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsNone(json.loads(proc.stdout)["reference_match"])
        self.assertIn('"reference_match": null', proc.stdout)

    def test_huge_budget_no_overruns(self):
        rep = self.report(self.project, "--budget-ms", "1000000")
        self.assertEqual(rep["overruns"], 0)

    def test_tiny_budget_finds_overruns(self):
        rep = self.report(self.project, "--budget-ms", "0.000000001")
        self.assertGreater(rep["overruns"], 0)

    def test_corrupted_reference_exit_1(self):
        ref = self.render_ref("corrupt.wav")
        size = os.path.getsize(ref)
        with open(ref, "r+b") as f:  # flip one byte in the data chunk
            f.seek(size // 2)
            b = f.read(1)
            f.seek(size // 2)
            f.write(bytes([b[0] ^ 0xFF]))
        proc = self.run_tool(self.project, "--reference", ref)
        self.assertEqual(proc.returncode, 1)
        rep = json.loads(proc.stdout)  # the report is still printed on exit 1
        self.assertIs(rep["reference_match"], False)
        self.assertTrue(rep["output_identical"])

    def test_repeated_option_last_wins(self):
        rep = self.report(self.project, "--block", "64", "--block", "128")
        self.assertEqual(rep["block_frames"], 128)
        self.assertEqual(rep["blocks"], math.ceil(rep["total_frames"] / 128))

    def test_block_sizes_ceil_and_default_budget(self):
        rep7 = self.report(self.project, "--block", "7")
        self.assertEqual(rep7["blocks"], math.ceil(rep7["total_frames"] / 7))
        self.assertEqual(rep7["blocks"], 68572)
        rep64 = self.report(self.project, "--block", "64")
        self.assertEqual(rep64["block_frames"], 64)
        self.assertIn('"budget_ms": 1.333333,', self.run_tool(self.project, "--block", "64").stdout)

    def test_usage_errors_exit_2_no_stdout(self):
        bad_cases = [
            [],  # no PROJECT at all
            [self.project, "--block", "0"],  # out of range
            [self.project, "--block", "8.0"],
            [self.project, "--block", "+8"],
            [self.project, "--block", "128abc"],
            [self.project, "--budget-ms", "inf"],
            [self.project, "--budget-ms", "nan"],
            [self.project, "--frobnicate", "x"],  # unknown option
            [self.project, "--reference"],  # missing option value
            [self.project, self.project],  # second positional
        ]
        for args in bad_cases:
            with self.subTest(args=args):
                proc = self.run_tool(*args)
                self.assertEqual(proc.returncode, 2)
                self.assertEqual(proc.stdout, "")
                self.assertIn("error:", proc.stderr)

    def test_valid_budget_scientific_notation(self):
        out = self.run_tool(self.project, "--budget-ms", "1e3").stdout
        self.assertIn('"budget_ms": 1000.000000,', out)

    def test_missing_project_exit_3_no_stdout(self):
        proc = self.run_tool(os.path.join(self.tmpdir, "nope.opj"))
        self.assertEqual(proc.returncode, 3)
        self.assertEqual(proc.stdout, "")
        self.assertIn("error: project not found:", proc.stderr)


if __name__ == "__main__":
    unittest.main()
