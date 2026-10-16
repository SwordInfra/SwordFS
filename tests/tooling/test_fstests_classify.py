#!/usr/bin/env python3
"""Focused tests for fstests detailed FAIL evidence classification."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts/conformance/fstests/classify.py"
SPEC = importlib.util.spec_from_file_location("fstests_classify", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
CLASSIFY = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CLASSIFY
SPEC.loader.exec_module(CLASSIFY)


class FstestsFailureEvidenceTest(unittest.TestCase):
    def test_kernel_local_only_pass_does_not_promote_distributed_support(self) -> None:
        for test in ("generic/131", "generic/478"):
            with self.subTest(test=test):
                gap = CLASSIFY.Gap("kernel_local_only", "#433", "PASS", "-", "-", "Only local locks are verified")
                for result, expected in (
                    ("PASS", "KERNEL_LOCAL_ONLY"),
                    ("FAIL", "BASELINE_RESULT_MISMATCH"),
                    ("NOTRUN", "BASELINE_RESULT_MISMATCH"),
                ):
                    actual = {test: CLASSIFY.RawResult(test, result)}
                    obs = CLASSIFY.classify({test}, actual, set(), {test: gap})[0]
                    self.assertEqual(expected, obs.classification)
                    self.assertEqual(result != "PASS", obs.classification in CLASSIFY.BLOCKING)

                for kwargs, expected in (
                    ({"crash_tests": {test}}, "SWORD_FS_CRASH"),
                    ({"isolation_failure_tests": {test}}, "INFRASTRUCTURE"),
                ):
                    obs = CLASSIFY.classify(
                        {test}, {test: CLASSIFY.RawResult(test, "PASS")}, set(), {test: gap}, **kwargs
                    )[0]
                    self.assertEqual(expected, obs.classification)

    def test_remote_flock_504_proc_locks_assumption_is_not_distributed_conformance(self) -> None:
        test = "generic/504"
        gap = CLASSIFY.Gap(
            "upstream_not_applicable",
            "-",
            "FAIL",
            "- output mismatch (see <FSTESTS_RESULT_DIR>/generic/504.out.bad)",
            "generic/504.out.bad",
            "FUSE remote flock is not recorded in /proc/locks",
        )
        evidence = "QA output created by 504\nlock info not found\nSilence is golden\n"
        actual = {test: CLASSIFY.RawResult(test, "FAIL", gap.expected_message)}
        result = CLASSIFY.classify(
            {test}, actual, set(), {test: gap}, expected_evidence={test: evidence}, actual_evidence={test: evidence}
        )[0]
        self.assertEqual(result.classification, "UPSTREAM_NOT_APPLICABLE")
        self.assertFalse(result.classification in CLASSIFY.BLOCKING)
        # The baseline remains exact: an unexpected PASS or different failure
        # must be investigated rather than accepted as the same observation.
        for unexpected in ("PASS", "NOTRUN"):
            result = CLASSIFY.classify({test}, {test: CLASSIFY.RawResult(test, unexpected)}, set(), {test: gap})[0]
            self.assertIn(result.classification, CLASSIFY.BLOCKING)

    def test_kernel_local_only_manifest_validation_and_true_xpass(self) -> None:
        header = "test\tcategory\tissue\texpected_result\texpected_message\texpected_evidence\treason\n"
        with tempfile.TemporaryDirectory() as temp_dir:
            path = pathlib.Path(temp_dir) / "known-gaps.tsv"
            for issue, result, message, evidence, valid in (
                ("#433", "PASS", "-", "-", True),
                ("-", "PASS", "-", "-", False),
                ("#433", "FAIL", "-", "-", False),
                ("#433", "NOTRUN", "-", "-", False),
                ("#433", "PASS", "unexpected", "-", False),
                ("#433", "PASS", "-", "generic/131.out.bad", False),
            ):
                with self.subTest(issue=issue, result=result, message=message, evidence=evidence):
                    path.write_text(
                        header + f"generic/131\tkernel_local_only\t{issue}\t{result}\t{message}\t{evidence}\tlocal pass only\n",
                        encoding="utf-8",
                    )
                    if valid:
                        self.assertEqual("PASS", CLASSIFY.load_gaps(path)["generic/131"].expected_result)
                    else:
                        with self.assertRaises(CLASSIFY.BaselineError):
                            CLASSIFY.load_gaps(path)

        test = "generic/999"
        gap = CLASSIFY.Gap("known_unsupported", "#255", "FAIL", "some failure", "-", "missing capability")
        actual = {test: CLASSIFY.RawResult(test, "PASS")}
        self.assertEqual("XPASS", CLASSIFY.classify({test}, actual, set(), {test: gap})[0].classification)

    def test_shared_upstream_tool_root_normalizes_to_original_harness_evidence(self) -> None:
        # Reusing the exact pinned tools must not require changing the known
        # semantic baseline: only the upstream executable's location moved.
        old = (
            "QA output created by 184\n"
            "/tmp/swordfs-fstests-ABCDEF/xfstests/tests/generic/184: line 21: "
            "/tmp/swordfs-fstests-ABCDEF/test/null: Permission denied\n"
        )
        new = (
            "QA output created by 184\n"
            "/opt/swordfs-fstests-tools/xfstests/tests/generic/184: line 21: "
            "/tmp/swordfs-fstests-GHIJKL/test/null: Permission denied\n"
        )
        self.assertEqual(
            CLASSIFY.normalize_failure_evidence(old, "generic/184.out.bad"),
            CLASSIFY.normalize_failure_evidence(new, "generic/184.out.bad"),
        )
        self.assertEqual(
            CLASSIFY.normalize_message(
                "/opt/swordfs-fstests-tools/xfstests/src/file_attr not built"
            ),
            "<FSTESTS_WORK_DIR>/xfstests/src/file_attr not built",
        )

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

    def test_mountfail_normalization_preserves_non_timestamp_failure_detail(self) -> None:
        raw = (
            '"/usr/bin/mount -t fuse.swordfs /tmp/swordfs-fstests-AAAAAA/scratch" '
            "failed at Mon Sep 28 06:16:16 UTC 2026: permission denied\n"
        )

        normalized = CLASSIFY.normalize_failure_evidence(raw, "generic/294.mountfail")

        self.assertEqual(
            '"/usr/bin/mount -t fuse.swordfs <FSTESTS_WORK_DIR>/scratch" '
            "failed at Mon Sep 28 06:16:16 UTC 2026: permission denied\n",
            normalized,
        )

    def test_checked_in_fail_evidence_inventory_matches_baseline(self) -> None:
        gaps = CLASSIFY.load_gaps(ROOT / "conformance/fstests/known-gaps.tsv")

        evidence = CLASSIFY.load_expected_evidence(ROOT / "conformance/fstests/fail-evidence", gaps)

        expected_tests = {test for test, gap in gaps.items() if gap.expected_evidence != "-"}
        self.assertEqual(expected_tests, set(evidence))

    def test_fail_evidence_inventory_rejects_missing_and_orphan_files(self) -> None:
        gap = CLASSIFY.Gap(
            "known_unsupported",
            "#255",
            "FAIL",
            "- output mismatch (see <FSTESTS_RESULT_DIR>/generic/131.out.bad)",
            "generic/131.out.bad",
            "locks are not implemented",
        )
        gaps = {"generic/131": gap}

        with tempfile.TemporaryDirectory() as temp_dir:
            evidence_dir = pathlib.Path(temp_dir)
            with self.assertRaisesRegex(CLASSIFY.BaselineError, "missing checked-in FAIL evidence"):
                CLASSIFY.load_expected_evidence(evidence_dir, gaps)

            expected = evidence_dir / "generic/131.out.bad"
            expected.parent.mkdir(parents=True)
            expected.write_text("expected evidence\n", encoding="utf-8")
            orphan = evidence_dir / "generic/999.out.bad"
            orphan.write_text("orphaned evidence\n", encoding="utf-8")
            with self.assertRaisesRegex(CLASSIFY.BaselineError, "orphaned checked-in FAIL evidence"):
                CLASSIFY.load_expected_evidence(evidence_dir, gaps)

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

    def test_bootstrap_writes_normalized_detailed_fail_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            result_dir = root / "results"
            output_dir = root / "output"
            evidence_path = pathlib.Path("generic/131.out.bad")
            source = result_dir / evidence_path
            source.parent.mkdir(parents=True)
            source.write_text(
                "QA output created by 131\n"
                "/tmp/swordfs-fstests-ABCDEF/test/lock: Function not implemented\n",
                encoding="utf-8",
            )
            output_dir.mkdir()
            observation = CLASSIFY.Observation(
                "generic/131",
                "FAIL",
                "UNEXPECTED_FAIL",
                "- output mismatch "
                "(see /home/runner/work/SwordFS/SwordFS/build/fstests-conformance/raw/results/generic/131.out.bad)",
            )

            CLASSIFY.write_bootstrap(output_dir, [observation], result_dir)

            bootstrap = (output_dir / "bootstrap-gaps.tsv").read_text(encoding="utf-8")
            normalized = (output_dir / "bootstrap-fail-evidence" / evidence_path).read_text(encoding="utf-8")
            self.assertIn("\tgeneric/131.out.bad\tclassify root cause", bootstrap)
            self.assertEqual(
                "QA output created by 131\n"
                "<FSTESTS_WORK_DIR>/test/lock: Function not implemented\n",
                normalized,
            )


if __name__ == "__main__":
    unittest.main()
