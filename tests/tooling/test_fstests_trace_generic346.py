#!/usr/bin/env python3
"""Diagnostic option leaves the pinned test's size/subcase sequence intact."""
from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("fstests_trace_generic346", ROOT / "scripts/conformance/fstests/trace_generic346.py")
assert SPEC and SPEC.loader
TRACE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = TRACE
SPEC.loader.exec_module(TRACE)


class TraceGeneric346Test(unittest.TestCase):
    def test_all_three_original_binary_launches_are_traced_exactly_once(self) -> None:
        lines = [f"$here/src/holetest -f -w $SCRATCH_MNT/testfile {n}\n" for n in (1,16,256)]
        script = "# pinned test\n" + "".join(lines) + "status=0\nexit\n"
        output = TRACE.instrument(script, pathlib.Path('/tmp/trace'))
        self.assertEqual(output.count("strace -f"), 3)
        for n in (1,16,256):
            self.assertIn(f"$here/src/holetest -f -w $SCRATCH_MNT/testfile {n}\n",output)
            self.assertIn(f"holetest-{n}MiB.syscalls",output)
        self.assertTrue(output.endswith("status=0\nexit\n"))

    def test_extracts_all_nine_test_modes_and_reports_timeout_partial(self) -> None:
        trace = """123 1799999999.000000 openat(AT_FDCWD, "/tmp/scratch/testfile", O_RDWR|O_EXCL|O_CREAT, 0644) = 3
123 1799999999.201000 unlink("/tmp/scratch/testfile") = 0
123 1800000000.000000 openat(AT_FDCWD, "/tmp/scratch/testfile", O_RDWR|O_EXCL|O_CREAT, 0644) = 3
123 1800000000.521000 unlink("/tmp/scratch/testfile") = 0
123 1800000001.000000 openat(AT_FDCWD, "/tmp/scratch/testfile", O_RDWR|O_EXCL|O_CREAT, 0644) = 3
"""
        result = TRACE.phase_times(trace)
        self.assertEqual([r["phase"] for r in result], ["zero-filled", "posix_fallocate", "ftruncate"])
        self.assertEqual([r["seconds"] for r in result], [0.201, 0.521, None])

    def test_mutated_upstream_commands_fail_closed(self) -> None:
        with self.assertRaisesRegex(ValueError, "changed"):
            TRACE.instrument("$here/src/holetest -f -w $SCRATCH_MNT/testfile 1\n", pathlib.Path('/tmp/trace'))


if __name__ == "__main__":
    unittest.main()
