#!/usr/bin/env python3
"""Deterministic checks for opt-in #515 diagnostics (not a PASS waiver)."""
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
    def test_measured_deltas_without_confusing_s3_usage_for_traffic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            raw = root / "raw"
            raw.mkdir()
            (root / "environment.json").write_text(json.dumps({"chunk_type": "chunk_slice", "slice_rmw_strategy": "fixed-1m"}))
            (raw / "redis-commandstats-before.txt").write_text("cmdstat_rpush:calls=20,usec=3\ncmdstat_lrange:calls=1,usec=2\n")
            (raw / "redis-commandstats-after.txt").write_text("cmdstat_rpush:calls=38,usec=3\ncmdstat_lrange:calls=7,usec=2\ncmdstat_exec:calls=4,usec=4\n")
            (raw / "redis-stats-before.txt").write_text("total_commands_processed:200\n")
            (raw / "redis-stats-after.txt").write_text("total_commands_processed:512\n")
            (raw / "check-status.tsv").write_text("generic/346\t0\n")
            (raw / "s3-scratch-volume-usage.json").write_text('{"size": 9999, "objects": 2}')
            (root / "swordfs-test.log").write_text("normal\n")
            (root / "swordfs-scratch.log").write_text("local RELEASE failed for ino=3\n")
            result = MODULE.run_arm(root)
            self.assertEqual(result["redis_total_commands_delta"], 312)
            self.assertEqual(result["redis_commands_delta"]["rpush"], 18)
            self.assertEqual(result["redis_commands_delta"]["exec"], 4)
            self.assertEqual(result["s3_final_volume_usage"]["scratch"]["size"], 9999)
            self.assertEqual(len(result["writeback_errors"]), 1)
            self.assertIn("NOT GET/PUT traffic", result["note"])

    def test_captures_actual_daemon_backend_timing_snapshots(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "raw").mkdir()
            (root / "swordfs-test.log").write_text(
                'I tool: SWORDFS_IO_TIMING {"s3_get":{"calls":2,"microseconds":150}}\n'
            )
            (root / "swordfs-scratch.log").write_text(
                'I tool: SWORDFS_IO_TIMING {"s3_put":{"calls":3,"microseconds":900}}\n'
            )
            timing = MODULE.run_arm(root)["backend_timing"]
            self.assertEqual(timing["test"]["s3_get"]["microseconds"], 150)
            self.assertEqual(timing["scratch"]["s3_put"]["calls"], 3)

    def test_service_time_shares_and_transaction_retry_diagnostics(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            (root / "raw").mkdir()
            (root / "swordfs-test.log").write_text(
                'SWORDFS_IO_TIMING ' + json.dumps({
                    "data": {"get": {"calls": 1, "us": 100, "bytes": 2},
                             "put": {"calls": 2, "us": 300, "bytes": 6}},
                    "metadata": {"calls": 5, "us": 600},
                    "transaction": {"calls": 3, "us": 550,
                                    "attempts": 8, "watch_conflicts": 4, "backoff_us": 200},
                }) + '\n'
            )
            (root / "swordfs-scratch.log").write_text("normal\n")
            result = MODULE.run_arm(root)
            self.assertEqual(result["service_time_us"], {"data": 400, "metadata": 600})
            self.assertEqual(result["service_share_pct"], {"data": 40.0, "metadata": 60.0})
            self.assertEqual(result["transaction_diagnostics"], {
                "calls": 3, "us": 550, "attempts": 8,
                "watch_conflicts": 4, "backoff_us": 200,
            })
            self.assertEqual(result["data_traffic_bytes"], {"get": 2, "put": 6})


if __name__ == "__main__":
    unittest.main()
