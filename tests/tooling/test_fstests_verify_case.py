#!/usr/bin/env python3
"""Strictly verify upstream test outcome and SwordFS writeback health."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/conformance/fstests/verify_case.py"
SPEC = importlib.util.spec_from_file_location("fstests_verify_case", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
VERIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = VERIFY
SPEC.loader.exec_module(VERIFY)


class VerifyCaseTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.output = pathlib.Path(self.tmp.name)
        (self.output / "raw/results").mkdir(parents=True)
        (self.output / "environment.json").write_text(json.dumps({"chunk_type": "chunk_slice"}))
        (self.output / "shard.json").write_text(json.dumps({"planned_tests": ["generic/346"]}))
        (self.output / "raw/check-status.tsv").write_text("generic/346\t0\n")
        (self.output / "raw/results/result.xml").write_text(
            '<testsuites><testsuite tests="1"><testcase name="generic/346"/></testsuite></testsuites>'
        )
        (self.output / "swordfs-test.log").write_text("I1027 mounted\n")
        (self.output / "swordfs-scratch.log").write_text("I1027 mounted\n")

    def test_clean_actual_xunit_and_daemon_logs_pass(self) -> None:
        VERIFY.verify(self.output, "chunk_slice", "generic/346")

    def test_fail_closed_on_internal_flush_error_despite_upstream_pass(self) -> None:
        with (self.output / "swordfs-scratch.log").open("a") as handle:
            handle.write("E1027 FileReadWriter.cpp:106] FileReadWriter::Flush chunk FAILED: "
                         "ino=8 chunk=3 — Redis transaction retry limit exceeded\n")
        with self.assertRaisesRegex(ValueError, "Flush chunk FAILED"):
            VERIFY.verify(self.output, "chunk_slice", "generic/346")

    def test_fail_closed_on_release_writeback_error(self) -> None:
        with (self.output / "swordfs-test.log").open("a") as handle:
            handle.write("W1027 Vfs.cpp:589] local RELEASE failed for ino=8: Redis transaction retry limit exceeded\n")
        with self.assertRaisesRegex(ValueError, "local RELEASE failed"):
            VERIFY.verify(self.output, "chunk_slice", "generic/346")

    def test_missing_daemon_log_does_not_silently_pass(self) -> None:
        (self.output / "swordfs-scratch.log").unlink()
        with self.assertRaises(FileNotFoundError):
            VERIFY.verify(self.output, "chunk_slice", "generic/346")

    def test_other_warnings_do_not_false_positive(self) -> None:
        (self.output / "swordfs-scratch.log").write_text("W1027 unrelated informational warning\n")
        VERIFY.verify(self.output, "chunk_slice", "generic/346")



if __name__ == "__main__":
    unittest.main()
