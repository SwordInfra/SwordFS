#!/usr/bin/env python3
"""Status publication must not turn failed CI into a green Slice report."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ConformanceStatusPublisherTest(unittest.TestCase):
    def publish(
        self, suite: str, outcome: str, *, with_result: bool = True, blocking_count: int = 0,
        with_report: bool = False,
    ) -> tuple[int, dict, str, dict]:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)
            (path / "history.json").write_text("[]", encoding="utf-8")
            result = path / "result.json"
            if with_result:
                metadata = {"swordfs_commit": "test-sha", "chunk_type": "chunk_slice"}
                if suite == "pjdfstest":
                    metadata["environment"] = {"chunk_type": "chunk_slice"}
                result.write_text(json.dumps({"metadata": metadata, "summary": {
                    "blocking_count": blocking_count, "raw_results": {"PASS": 1}, "classifications": {"PASS": 1},
                    "selected_count": 1, "executed_count": 1, "supported_count": 1,
                    "overall_support_percent": 100, "supported_regression_pass_percent": 100,
                    "classified_rate_percent": 100,
                }}), encoding="utf-8")
            if with_report:
                (path / "report.md").write_text("# Upstream classifier\n\nBaseline gate: **PASS**\n", encoding="utf-8")
            args = [
                sys.executable, str(ROOT / "scripts/conformance" / suite / "update-status.py"),
                "--result", str(result), "--report", str(path / "report.md"),
                "--history", str(path / "history.json"),
                "--output-dir", str(path), "--swordfs-commit", "test-sha",
                "--run-url", "https://example.test/actions/runs/42",
                "--infra-failure", f"{suite} job result: {outcome}",
                "--conformance-job-result", outcome,
                "--expected-chunk-type", "chunk_slice",
            ]
            if suite == "fstests":
                args.extend(["--run-id", "42"])
            proc = subprocess.run(args, capture_output=True, text=True, check=False)
            if proc.returncode:
                return proc.returncode, {}, proc.stderr, {}
            history = json.loads((path / "history.json").read_text(encoding="utf-8"))
            latest = json.loads((path / "latest.json").read_text(encoding="utf-8"))
            return proc.returncode, history[-1], (path / "status.md").read_text(encoding="utf-8"), latest

    def test_failed_cancelled_or_skipped_job_never_publishes_pass_from_clean_result(self) -> None:
        for suite in ("fstests", "pjdfstest"):
            for outcome in ("failure", "cancelled", "skipped"):
                with self.subTest(suite=suite, outcome=outcome):
                    returncode, entry, report, latest = self.publish(suite, outcome)
                    self.assertEqual(0, returncode, report)
                    self.assertEqual("INFRASTRUCTURE_FAILED", entry["status"])
                    self.assertEqual("chunk_slice", entry["chunk_type"])
                    self.assertEqual("INFRASTRUCTURE_FAILED", latest["status"])
                    self.assertEqual(outcome, latest["conformance_job_result"])
                    self.assertIn("CI NOT SUCCESSFUL", report)
                    self.assertIn(outcome, report)
                    self.assertNotIn("Latest run: **PASS**", report)
                    self.assertNotIn("Baseline gate: **PASS**", report)

    def test_failed_job_must_not_republish_partial_clean_classifier_report(self) -> None:
        for suite in ("fstests", "pjdfstest"):
            with self.subTest(suite=suite):
                returncode, entry, report, latest = self.publish(suite, "failure", with_report=True)
                self.assertEqual(0, returncode, report)
                self.assertEqual("INFRASTRUCTURE_FAILED", entry["status"])
                self.assertNotIn("Baseline gate: **PASS**", report)
                self.assertEqual("INFRASTRUCTURE_FAILED", latest["status"])

    def test_failed_job_with_real_test_regression_preserves_blocked_status(self) -> None:
        for suite, blocked in (("fstests", "BLOCKED"), ("pjdfstest", "FAILED")):
            with self.subTest(suite=suite):
                returncode, entry, report, latest = self.publish(suite, "failure", blocking_count=1)
                self.assertEqual(0, returncode, report)
                self.assertEqual(blocked, entry["status"])
                self.assertEqual(blocked, latest["status"])
                self.assertIn("failure", report)

    def test_successful_clean_job_publishes_pass(self) -> None:
        for suite in ("fstests", "pjdfstest"):
            with self.subTest(suite=suite):
                returncode, entry, _, latest = self.publish(suite, "success")
                self.assertEqual(0, returncode)
                self.assertEqual("PASS", entry["status"])
                self.assertEqual("PASS", latest["status"])
                self.assertEqual("success", latest["conformance_job_result"])

    def test_missing_report_cannot_pass_even_when_job_succeeded(self) -> None:
        for suite in ("fstests", "pjdfstest"):
            with self.subTest(suite=suite):
                returncode, entry, report, latest = self.publish(suite, "success", with_result=False)
                self.assertEqual(0, returncode, report)
                self.assertEqual("INFRASTRUCTURE_FAILED", entry["status"])
                self.assertEqual("INFRASTRUCTURE_FAILED", latest["status"])


if __name__ == "__main__":
    unittest.main()
