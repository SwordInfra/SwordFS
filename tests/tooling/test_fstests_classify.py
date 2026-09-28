#!/usr/bin/env python3
"""Focused tests for fstests detailed FAIL evidence classification."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts/conformance/fstests/classify.py"
SPEC = importlib.util.spec_from_file_location("fstests_classify", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
CLASSIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CLASSIFY
SPEC.loader.exec_module(CLASSIFY)


class FstestsFailureEvidenceTest(unittest.TestCase):
    def test_out_bad_normalization_removes_run_paths_and_absolute_stat_times(self) -> None:
        first = """QA output created by 120
  File: \"/tmp/swordfs-fstests-AAAAAA/scratch/testfile\"
Access: Mon Sep 28 06:00:01 2026(00000.00:00:00)
Modify: Mon Sep 28 06:00:06 2026(00000.00:00:05)
"""
        second = """QA output created by 120
  File: \"/tmp/swordfs-fstests-BBBBBB/scratch/testfile\"
Access: Mon Sep 28 07:10:11 2026(00000.00:00:00)
Modify: Mon Sep 28 07:10:16 2026(00000.00:00:05)
"""

        first_normalized = CLASSIFY.normalize_failure_evidence(first, "generic/120.out.bad")
        second_normalized = CLASSIFY.normalize_failure_evidence(second, "generic/120.out.bad")

        self.assertEqual(first_normalized, second_normalized)
        self.assertIn("<FSTESTS_WORK_DIR>/scratch/testfile", first_normalized)
        self.assertIn("Modify: <TIMESTAMP>(00000.00:00:05)", first_normalized)

    def test_out_bad_normalization_compacts_long_contiguous_indexed_failures(self) -> None:
        repeated = "\n".join(
            f"open_by_handle(/tmp/swordfs-fstests-AAAAAA/test/426-dir/file{i:06d}) "
            "returned 116 incorrectly on a linked file!"
            for i in range(32)
        )
        raw = f"QA output created by 426\n{repeated}\n"

        normalized = CLASSIFY.normalize_failure_evidence(raw, "generic/426.out.bad")

        self.assertEqual(
            "QA output created by 426\n"
            "open_by_handle(<FSTESTS_WORK_DIR>/test/426-dir/file{000000..000031}) "
            "returned 116 incorrectly on a linked file! [count=32]\n",
            normalized,
        )

    def test_out_bad_compaction_splits_on_semantic_change_and_index_gap(self) -> None:
        lines = [
            f"open_by_handle(/tmp/swordfs-fstests-AAAAAA/test/426-dir/file{i:06d}) "
            "returned 116 incorrectly on a linked file!"
            for i in range(20)
        ]
        lines.append(
            "open_by_handle(/tmp/swordfs-fstests-AAAAAA/test/426-dir/file000020) "
            "returned 5 incorrectly on a linked file!"
        )
        lines.extend(
            f"open_by_handle(/tmp/swordfs-fstests-AAAAAA/test/426-dir/file{i:06d}) "
            "returned 116 incorrectly on a linked file!"
            for i in range(21, 41)
        )
        # Skip file000041 so the next run must remain a separate range.
        lines.extend(
            f"open_by_handle(/tmp/swordfs-fstests-AAAAAA/test/426-dir/file{i:06d}) "
            "returned 116 incorrectly on a linked file!"
            for i in range(42, 62)
        )

        normalized = CLASSIFY.normalize_failure_evidence("\n".join(lines) + "\n", "generic/426.out.bad")

        self.assertIn("file{000000..000019}", normalized)
        self.assertIn("file000020) returned 5", normalized)
        self.assertIn("file{000021..000040}", normalized)
        self.assertIn("file{000042..000061}", normalized)
        self.assertNotIn("file{000021..000061}", normalized)

    def test_mountfail_normalization_removes_only_harness_run_noise(self) -> None:
        first = """\"/usr/bin/mount -t fuse.swordfs /tmp/swordfs-fstests-AAAAAA/scratch\" failed at Mon Sep 28 06:16:16 UTC 2026
[  427.561548] run fstests generic/294 at 2026-09-28 06:15:46
[  587.022560] sh (177912): drop_caches: 3
mount: semantic failure text
"""
        second = """\"/usr/bin/mount -t fuse.swordfs /tmp/swordfs-fstests-BBBBBB/scratch\" failed at Mon Sep 28 07:26:26 UTC 2026
[ 1027.100000] run fstests generic/281 at 2026-09-28 07:25:46
[ 1187.200000] sh (277912): drop_caches: 3
[ 1188.200000] sh (277913): drop_caches: 3
mount: semantic failure text
"""

        first_normalized = CLASSIFY.normalize_failure_evidence(first, "generic/294.mountfail")
        second_normalized = CLASSIFY.normalize_failure_evidence(second, "generic/294.mountfail")

        self.assertEqual(first_normalized, second_normalized)
        self.assertNotIn("run fstests", first_normalized)
        self.assertNotIn("drop_caches", first_normalized)
        self.assertIn("mount: semantic failure text", first_normalized)

    def test_generic_output_mismatch_accepts_unchanged_detailed_evidence(self) -> None:
        test = "generic/131"
        expected_message = "- output mismatch (see <FSTESTS_RESULT_DIR>/generic/131.out.bad)"
        gap = CLASSIFY.Gap(
            "known_unsupported",
            "#255",
            "FAIL",
            expected_message,
            "generic/131.out.bad",
            "locks are not implemented",
        )
        actual = CLASSIFY.RawResult(
            test,
            "FAIL",
            "- output mismatch (see /home/runner/work/SwordFS/SwordFS/build/fstests-conformance/raw/results/generic/131.out.bad)",
        )

        observations = CLASSIFY.classify(
            {test},
            {test: actual},
            set(),
            {test: gap},
            expected_evidence={test: "same evidence\n"},
            actual_evidence={test: "same evidence\n"},
        )

        self.assertEqual("KNOWN_UNSUPPORTED", observations[0].classification)

    def test_generic_output_mismatch_blocks_changed_detailed_evidence(self) -> None:
        test = "generic/131"
        message = "- output mismatch (see <FSTESTS_RESULT_DIR>/generic/131.out.bad)"
        gap = CLASSIFY.Gap(
            "known_unsupported",
            "#255",
            "FAIL",
            message,
            "generic/131.out.bad",
            "locks are not implemented",
        )
        actual = CLASSIFY.RawResult(test, "FAIL", message)

        observations = CLASSIFY.classify(
            {test},
            {test: actual},
            set(),
            {test: gap},
            expected_evidence={test: "expected semantic evidence\n"},
            actual_evidence={test: "different semantic evidence\n"},
        )

        self.assertEqual("BASELINE_EVIDENCE_MISMATCH", observations[0].classification)
        self.assertIn("detailed FAIL evidence changed", observations[0].message)

    def test_xunit_message_mismatch_still_takes_precedence(self) -> None:
        test = "generic/131"
        expected_message = "- output mismatch (see <FSTESTS_RESULT_DIR>/generic/131.out.bad)"
        gap = CLASSIFY.Gap(
            "known_unsupported",
            "#255",
            "FAIL",
            expected_message,
            "generic/131.out.bad",
            "locks are not implemented",
        )

        observations = CLASSIFY.classify(
            {test},
            {test: CLASSIFY.RawResult(test, "FAIL", "a different XUnit failure class")},
            set(),
            {test: gap},
            expected_evidence={test: "expected semantic evidence\n"},
            actual_evidence={test: "different semantic evidence\n"},
        )

        self.assertEqual("BASELINE_REASON_MISMATCH", observations[0].classification)

    def test_missing_detailed_evidence_is_blocking(self) -> None:
        test = "generic/131"
        message = "- output mismatch (see <FSTESTS_RESULT_DIR>/generic/131.out.bad)"
        gap = CLASSIFY.Gap(
            "known_unsupported",
            "#255",
            "FAIL",
            message,
            "generic/131.out.bad",
            "locks are not implemented",
        )

        observations = CLASSIFY.classify(
            {test},
            {test: CLASSIFY.RawResult(test, "FAIL", message)},
            set(),
            {test: gap},
            expected_evidence={test: "expected semantic evidence\n"},
            actual_evidence={},
        )

        self.assertEqual("BASELINE_EVIDENCE_MISMATCH", observations[0].classification)
        self.assertIn("missing detailed FAIL evidence", observations[0].message)


if __name__ == "__main__":
    unittest.main()
