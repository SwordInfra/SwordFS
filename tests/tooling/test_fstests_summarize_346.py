#!/usr/bin/env python3
"""Deterministic tests of genuine COW/Slice generic/346 diagnostics."""
from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts/conformance/fstests"
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("summarize_346", SCRIPTS / "summarize_346.py")
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class SummarizeTest(unittest.TestCase):
    def test_command_deltas_without_confusing_retained_s3_size_with_traffic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            raw = root / "raw"
            raw.mkdir()
            (root / "environment.json").write_text(json.dumps({"chunk_type": "chunk_slice"}))
            (raw / "redis-commandstats-before.txt").write_text("cmdstat_rpushx:calls=20,usec=3\n")
            (raw / "redis-commandstats-after.txt").write_text(
                "cmdstat_rpushx:calls=38,usec=3\ncmdstat_exec:calls=4,usec=4\n"
            )
            (raw / "redis-stats-before.txt").write_text("total_commands_processed:200\n")
            (raw / "redis-stats-after.txt").write_text("total_commands_processed:512\n")
            (raw / "check-status.tsv").write_text("generic/346\t0\n")
            (raw / "s3-scratch-volume-usage.json").write_text('{"size": 9999, "objects": 2}')
            (root / "swordfs-test.log").write_text("normal\n")
            (root / "swordfs-scratch.log").write_text("local RELEASE failed for ino=3\n")
            result = MODULE.run_arm(root)
            self.assertEqual(result["redis_total_commands_delta"], 312)
            self.assertEqual(result["redis_commands_delta"]["rpushx"], 18)
            self.assertEqual(result["redis_commands_delta"]["exec"], 4)
            self.assertEqual(result["s3_final_volume_usage"]["scratch"]["size"], 9999)
            self.assertEqual(len(result["writeback_errors"]), 1)
            self.assertIn("not GET/PUT traffic", result["note"])


if __name__ == "__main__":
    unittest.main()
