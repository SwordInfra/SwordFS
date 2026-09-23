#!/usr/bin/env python3
"""Integration assertions for the semantic symbol-audit fixture."""

from __future__ import annotations

import argparse
import copy
import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "static-analysis"))

import symbol_audit  # noqa: E402


def _parse_args() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--build-dir", type=Path, required=True)
    return parser.parse_known_args()


ARGS, UNITTEST_ARGS = _parse_args()
FIXTURE_ROOT = Path(__file__).resolve().parent / "fixture"
FIXTURE_SUPPRESSIONS = FIXTURE_ROOT / "suppressions.json"


class SymbolAuditFixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.report = symbol_audit.analyze(
            FIXTURE_ROOT,
            ARGS.build_dir,
            FIXTURE_SUPPRESSIONS,
        )
        cls.findings = cls.report["findings"]

    def _find(self, spelling: str, category: str) -> list[dict[str, object]]:
        return [
            finding
            for finding in self.findings
            if finding["spelling"] == spelling and finding["category"] == category
        ]

    def test_zero_reference_function_is_blocking(self) -> None:
        findings = self._find("DeadFunction", "zero-reference-production-symbol")
        self.assertEqual(1, len(findings))
        self.assertEqual("high", findings[0]["confidence"])
        self.assertEqual(0, findings[0]["production_reference_count"])
        self.assertEqual(0, findings[0]["test_reference_count"])

    def test_unused_alias_is_blocking(self) -> None:
        findings = self._find("DeadAlias", "unused-production-alias")
        self.assertEqual(1, len(findings))
        self.assertEqual("high", findings[0]["confidence"])

    def test_test_only_function_is_blocking(self) -> None:
        findings = self._find("TestOnlyFunction", "test-only-production-symbol")
        self.assertEqual(1, len(findings))
        self.assertEqual("high", findings[0]["confidence"])
        self.assertGreater(findings[0]["test_reference_count"], 0)
        self.assertEqual(0, findings[0]["production_reference_count"])

    def test_overloads_are_distinguished_semantically(self) -> None:
        findings = self._find("Overloaded", "test-only-production-symbol")
        self.assertEqual(1, len(findings))
        self.assertIn("string_view", findings[0]["symbol"])

    def test_test_only_alias_is_blocking(self) -> None:
        findings = self._find("TestAlias", "test-only-production-symbol")
        self.assertEqual(1, len(findings))
        self.assertEqual("high", findings[0]["confidence"])

    def test_virtual_and_registered_surfaces_are_not_false_positives(self) -> None:
        reported_spellings = {finding["spelling"] for finding in self.findings}
        self.assertNotIn("Run", reported_spellings)
        self.assertNotIn("FrameworkCallback", reported_spellings)
        self.assertNotIn("RegisteredFactory", reported_spellings)
        self.assertNotIn("ProductionUsed", reported_spellings)
        self.assertNotIn("ProductionConsumer", reported_spellings)
        self.assertNotIn("main", reported_spellings)
        self.assertNotIn("operator==", reported_spellings)

    def test_library_lock_protocol_methods_are_not_blocking(self) -> None:
        findings = [
            finding
            for finding in self.findings
            if finding["spelling"] in {"lock", "unlock"}
        ]
        self.assertEqual(2, len(findings))
        for finding in findings:
            self.assertEqual("indirect-protocol-method", finding["category"])
            self.assertEqual("review", finding["confidence"])

    def test_exact_suppression_marks_matching_finding(self) -> None:
        finding = copy.deepcopy(
            self._find("DeadFunction", "zero-reference-production-symbol")[0]
        )
        suppression = {
            "category": finding["category"],
            "usr": finding["usr"],
            "symbol": finding["symbol"],
            "reason": "fixture verifies exact semantic suppression matching",
        }

        symbol_audit._apply_suppressions([finding], [suppression])

        self.assertTrue(finding["suppressed"])
        self.assertEqual(suppression["reason"], finding["suppression_reason"])

    def test_stale_suppression_is_rejected(self) -> None:
        suppression = {
            "category": "zero-reference-production-symbol",
            "usr": "c:@F@does_not_exist#",
            "symbol": "fixture::DoesNotExist()",
            "reason": "fixture stale suppression",
        }

        with self.assertRaises(symbol_audit.AuditError):
            symbol_audit._apply_suppressions([], [suppression])

    def test_parallel_analysis_matches_single_process(self) -> None:
        parallel_report = symbol_audit.analyze(
            FIXTURE_ROOT,
            ARGS.build_dir,
            FIXTURE_SUPPRESSIONS,
            jobs=2,
        )

        self.assertEqual(self.report["summary"], parallel_report["summary"])
        self.assertEqual(self.report["findings"], parallel_report["findings"])

    def test_invalid_parallelism_is_rejected(self) -> None:
        with self.assertRaisesRegex(symbol_audit.AuditError, "jobs must be at least 1"):
            symbol_audit.analyze(
                FIXTURE_ROOT,
                ARGS.build_dir,
                REPO_ROOT / "scripts" / "static-analysis" / "suppressions.json",
                jobs=0,
            )

    def test_compile_arguments_remove_driver_only_flags(self) -> None:
        source = FIXTURE_ROOT / "src" / "Production.cpp"
        unit = symbol_audit.CompileUnit(
            directory=FIXTURE_ROOT,
            source=source,
            arguments=(
                "ccache",
                "/usr/bin/c++",
                "-c",
                source.as_posix(),
                "-o",
                "Production.o",
                "-MD",
                "-MF",
                "Production.d",
                "-Iinclude",
            ),
        )

        self.assertEqual(["-Iinclude"], symbol_audit._compile_arguments(unit))

    def test_empty_compile_command_is_rejected(self) -> None:
        unit = symbol_audit.CompileUnit(
            directory=FIXTURE_ROOT,
            source=FIXTURE_ROOT / "src" / "Production.cpp",
            arguments=("ccache",),
        )
        with self.assertRaisesRegex(symbol_audit.AuditError, "empty compile command"):
            symbol_audit._compile_arguments(unit)

    def test_suppression_file_is_fail_closed(self) -> None:
        valid = {
            "category": "zero-reference-production-symbol",
            "usr": "c:@F@dead#",
            "symbol": "fixture::Dead()",
            "reason": "intentional external ABI entry point",
        }
        invalid_cases = [
            ("{", "invalid suppression JSON"),
            (json.dumps({}), "must be a JSON array"),
            (json.dumps([{key: value for key, value in valid.items() if key != "reason"}]), "must contain exactly"),
            (json.dumps([{**valid, "reason": ""}]), "empty or non-string"),
        ]

        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "suppressions.json"
            for content, message in invalid_cases:
                with self.subTest(message=message):
                    path.write_text(content, encoding="utf-8")
                    with self.assertRaisesRegex(symbol_audit.AuditError, message):
                        symbol_audit._load_suppressions(path)

    def test_github_output_contract(self) -> None:
        finding = copy.deepcopy(
            self._find("DeadFunction", "zero-reference-production-symbol")[0]
        )
        output = io.StringIO()
        with mock.patch.dict(symbol_audit.os.environ, {"GITHUB_ACTIONS": "true"}), redirect_stdout(output):
            symbol_audit._emit_github_annotation(finding)

        annotation = output.getvalue()
        self.assertIn("::error file=", annotation)
        self.assertIn("zero-reference-production-symbol", annotation)

        with tempfile.TemporaryDirectory() as directory:
            summary_path = Path(directory) / "summary.md"
            with mock.patch.dict(
                symbol_audit.os.environ,
                {"GITHUB_STEP_SUMMARY": summary_path.as_posix()},
            ):
                symbol_audit._write_github_summary(
                    self.report,
                    Path(directory) / "static-audit.json",
                )
            summary = summary_path.read_text(encoding="utf-8")
            self.assertIn("SwordFS static symbol audit", summary)
            self.assertIn("Review candidates", summary)

    def test_cli_writes_report_and_returns_blocking_status(self) -> None:
        report = copy.deepcopy(self.report)
        self.assertGreater(report["summary"]["blocking_high_confidence"], 0)

        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            args = argparse.Namespace(
                repo_root=FIXTURE_ROOT,
                build_dir=ARGS.build_dir,
                suppressions=FIXTURE_SUPPRESSIONS,
                output=output,
                jobs=1,
                no_fail=False,
            )
            stdout = io.StringIO()
            with mock.patch.object(symbol_audit, "_parse_args", return_value=args), mock.patch.object(
                symbol_audit, "analyze", return_value=report
            ), redirect_stdout(stdout):
                self.assertEqual(1, symbol_audit.main())

            self.assertEqual(report, json.loads(output.read_text(encoding="utf-8")))
            self.assertIn("blocking=", stdout.getvalue())

    def test_cli_reports_configuration_error(self) -> None:
        args = argparse.Namespace(
            repo_root=FIXTURE_ROOT,
            build_dir=ARGS.build_dir,
            suppressions=FIXTURE_SUPPRESSIONS,
            output=FIXTURE_ROOT / "unused.json",
            jobs=1,
            no_fail=False,
        )
        stderr = io.StringIO()
        with mock.patch.object(symbol_audit, "_parse_args", return_value=args), mock.patch.object(
            symbol_audit,
            "analyze",
            side_effect=symbol_audit.AuditError("fixture configuration failure"),
        ), redirect_stderr(stderr):
            self.assertEqual(2, symbol_audit.main())

        self.assertIn("fixture configuration failure", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *UNITTEST_ARGS])
