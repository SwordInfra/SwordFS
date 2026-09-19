#!/usr/bin/env python3
"""Integration assertions for the semantic symbol-audit fixture."""

from __future__ import annotations

import argparse
import copy
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import symbol_audit  # noqa: E402


def _parse_args() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--build-dir", type=Path, required=True)
    return parser.parse_known_args()


ARGS, UNITTEST_ARGS = _parse_args()
FIXTURE_ROOT = Path(__file__).resolve().parent / "fixture"


class SymbolAuditFixtureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.report = symbol_audit.analyze(
            FIXTURE_ROOT,
            ARGS.build_dir,
            REPO_ROOT / "scripts" / "static_audit_suppressions.json",
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

    def test_test_only_function_is_review_only(self) -> None:
        findings = self._find("TestOnlyFunction", "test-only-production-symbol")
        self.assertEqual(1, len(findings))
        self.assertEqual("review", findings[0]["confidence"])
        self.assertGreater(findings[0]["test_reference_count"], 0)
        self.assertEqual(0, findings[0]["production_reference_count"])

    def test_overloads_are_distinguished_semantically(self) -> None:
        findings = self._find("Overloaded", "test-only-production-symbol")
        self.assertEqual(1, len(findings))
        self.assertIn("string_view", findings[0]["symbol"])

    def test_test_only_alias_is_visible_without_blocking(self) -> None:
        findings = self._find("TestAlias", "test-only-production-symbol")
        self.assertEqual(1, len(findings))
        self.assertEqual("review", findings[0]["confidence"])

    def test_virtual_and_registered_surfaces_are_not_false_positives(self) -> None:
        reported_spellings = {finding["spelling"] for finding in self.findings}
        self.assertNotIn("Run", reported_spellings)
        self.assertNotIn("FrameworkCallback", reported_spellings)
        self.assertNotIn("RegisteredFactory", reported_spellings)
        self.assertNotIn("ProductionUsed", reported_spellings)
        self.assertNotIn("ProductionConsumer", reported_spellings)
        self.assertNotIn("main", reported_spellings)

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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *UNITTEST_ARGS])
