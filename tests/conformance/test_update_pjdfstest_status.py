import json
import pathlib
import subprocess
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "update-pjdfstest-status.py"


class UpdatePjdfstestStatusTest(unittest.TestCase):
    def test_publishes_classified_result_and_history(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            result = root / "result.json"
            report = root / "report.md"
            history = root / "history.json"
            output = root / "out"
            result.write_text(
                json.dumps(
                    {
                        "metadata": {"swordfs_commit": "abc", "pjdfstest_commit": "def"},
                        "summary": {
                            "blocking_count": 0,
                            "overall_support_percent": 75.0,
                            "supported_regression_pass_percent": 100.0,
                            "classified_rate_percent": 100.0,
                            "supported_set_size": 12,
                            "counts": {"PASS": 12},
                            "by_category": {"open": {"pass": 10, "applicable": 12, "support_percent": 83.33}},
                        },
                    }
                ),
                encoding="utf-8",
            )
            report.write_text("# Current report\n", encoding="utf-8")

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--result",
                    str(result),
                    "--report",
                    str(report),
                    "--history",
                    str(history),
                    "--output-dir",
                    str(output),
                    "--swordfs-commit",
                    "abc",
                    "--run-url",
                    "https://example/run/1",
                ],
                check=True,
            )

            status = (output / "status.md").read_text(encoding="utf-8")
            self.assertIn("# Current report", status)
            self.assertIn("Historical main-branch trend", status)
            self.assertIn("https://example/run/1", status)
            payload = json.loads((output / "history.json").read_text(encoding="utf-8"))
            self.assertEqual("PASS", payload[-1]["status"])
            self.assertEqual(75.0, payload[-1]["overall_support_percent"])
            self.assertEqual(83.33, payload[-1]["by_category"]["open"]["support_percent"])

    def test_infrastructure_failure_replaces_stale_healthy_headline(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            history = root / "history.json"
            output = root / "out"
            history.write_text(
                json.dumps(
                    [
                        {
                            "timestamp": "2026-01-01T00:00:00+00:00",
                            "swordfs_commit": "healthy",
                            "status": "PASS",
                            "overall_support_percent": 80.0,
                            "supported_regression_pass_percent": 100.0,
                            "classified_rate_percent": 100.0,
                        }
                    ]
                ),
                encoding="utf-8",
            )

            subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--history",
                    str(history),
                    "--output-dir",
                    str(output),
                    "--swordfs-commit",
                    "broken",
                    "--run-url",
                    "https://example/run/2",
                    "--infra-failure",
                    "mount failed",
                ],
                check=True,
            )
            status = (output / "status.md").read_text(encoding="utf-8")
            self.assertIn("INFRASTRUCTURE FAILED", status)
            self.assertIn("mount failed", status)
            self.assertIn("healthy", status)


if __name__ == "__main__":
    unittest.main()
