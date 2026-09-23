import json
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "update-fstests-status.py"


class UpdateFstestsStatusTest(unittest.TestCase):
    def test_persists_and_renders_overall_support(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            result = root / "result.json"
            report = root / "report.md"
            history = root / "history.json"
            output = root / "status"
            result.write_text(
                json.dumps(
                    {
                        "metadata": {
                            "swordfs_commit": "abc123",
                            "fstests_commit": "fstests123",
                            "fstests_group": "generic/quick",
                        },
                        "summary": {
                            "blocking_count": 0,
                            "selected_count": 645,
                            "executed_count": 642,
                            "deferred_count": 3,
                            "supported_count": 102,
                            "known_gap_count": 540,
                            "supported_gate_percent": 100.0,
                            "overall_support_percent": 15.81,
                            "executed_classified_percent": 100.0,
                            "raw_results": {"PASS": 102, "FAIL": 25, "NOTRUN": 515, "DEFERRED": 3},
                            "classifications": {"PASS": 102},
                        },
                    }
                ),
                encoding="utf-8",
            )
            report.write_text("# SwordFS fstests conformance\n\n| Overall support | 15.81% |\n", encoding="utf-8")

            subprocess.run(
                [
                    "python3",
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
                    "abc123",
                    "--run-url",
                    "https://example.test/run/1",
                    "--run-id",
                    "1",
                ],
                check=True,
            )

            payload = json.loads((output / "history.json").read_text(encoding="utf-8"))
            self.assertEqual(15.81, payload[-1]["overall_support_percent"])
            status = (output / "status.md").read_text(encoding="utf-8")
            self.assertIn("| Overall support | 15.81% |", status)
            self.assertIn("| 15.81% | 102 | 102 | 25 | 515 | 3 | 540 |", status)


if __name__ == "__main__":
    unittest.main()
