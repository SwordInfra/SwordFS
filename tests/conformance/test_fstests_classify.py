import importlib.util
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "fstests_classify.py"
SPEC = importlib.util.spec_from_file_location("fstests_classify", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def _xunit(path: pathlib.Path, cases: list[tuple[str, str, str]]) -> None:
    lines = ['<?xml version="1.0" encoding="UTF-8"?>', '<testsuite name="xfstests">']
    for test, result, message in cases:
        lines.append(f'  <testcase name="{test}" time="1">')
        if result == "FAIL":
            lines.append(f'    <failure message="{message}" type="TestFail" />')
        elif result == "NOTRUN":
            lines.append(f'    <skipped message="{message}" />')
        lines.append("  </testcase>")
    lines.append("</testsuite>")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


class FstestsClassifyTest(unittest.TestCase):
    def test_swordfs_crash_loader_ignores_other_process_core_dumps(self):
        with tempfile.TemporaryDirectory() as temporary:
            result_dir = pathlib.Path(temporary)
            group = result_dir / "generic"
            group.mkdir()
            (group / "104.full").write_text(
                "Message: Process 123 (swordfs) of user 0 dumped core.\n",
                encoding="utf-8",
            )
            (group / "394.full").write_text(
                "Message: Process 456 (xfs_io) of user 0 dumped core.\n",
                encoding="utf-8",
            )
            self.assertEqual({"generic/104"}, MODULE.load_swordfs_crash_tests(result_dir))

    def test_swordfs_crash_overrides_known_gap_and_notrun(self):
        selected = {"generic/532"}
        actual = {"generic/532": MODULE.RawResult("generic/532", "NOTRUN", "statx unsupported")}
        gaps = {
            "generic/532": MODULE.Gap(
                "known_unsupported",
                "#252",
                "NOTRUN",
                "statx unsupported",
                "statx pending",
            )
        }
        result = MODULE.classify(selected, actual, set(), gaps, {}, crash_tests={"generic/532"})[0]
        self.assertEqual("SWORD_FS_CRASH", result.classification)
        self.assertIn("dumped core", result.message)

    def test_swordfs_crash_overrides_missing_xunit_result(self):
        selected = {"generic/104"}
        result = MODULE.classify(selected, {}, set(), {}, {}, crash_tests={"generic/104"})[0]
        self.assertEqual("SWORD_FS_CRASH", result.classification)

    def test_runner_isolation_failure_forces_infrastructure_even_for_known_gap(self):
        selected = {"generic/008"}
        actual = {"generic/008": MODULE.RawResult("generic/008", "FAIL", "output mismatch")}
        gaps = {
            "generic/008": MODULE.Gap(
                "known_semantic_defect",
                "#42",
                "FAIL",
                "output mismatch",
                "known semantic failure",
            )
        }
        result = MODULE.classify(selected, actual, set(), gaps, {}, isolation_failure_tests={"generic/008"})[0]
        self.assertEqual("INFRASTRUCTURE", result.classification)
        self.assertIn("restore testcase isolation", result.message)

    def test_isolation_failure_loader_maps_report_path_to_exact_test_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            result_dir = pathlib.Path(temporary)
            group = result_dir / "generic"
            group.mkdir()
            (group / "008.isolationfail").write_text("runner intervention\n", encoding="utf-8")
            self.assertEqual({"generic/008"}, MODULE.load_isolation_failure_tests(result_dir))

    def test_upstream_mountfail_remains_a_normal_known_gap(self):
        selected = {"generic/294"}
        message = "check: possible mount failures (see <FSTESTS_RESULT_DIR>/generic/294.mountfail)"
        actual = {"generic/294": MODULE.RawResult("generic/294", "FAIL", message)}
        gaps = {
            "generic/294": MODULE.Gap(
                "known_unsupported",
                "#999",
                "FAIL",
                message,
                "read-only remount is not supported",
            )
        }
        result = MODULE.classify(selected, actual, set(), gaps, {})[0]
        self.assertEqual("KNOWN_UNSUPPORTED", result.classification)

    def test_missing_result_dir_does_not_hide_harness_infrastructure_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = pathlib.Path(temporary) / "not-created-yet"
            self.assertEqual(set(), MODULE.load_isolation_failure_tests(missing))

    def test_message_normalization_canonicalizes_only_harness_work_dir(self):
        self.assertEqual(
            "FITRIM not supported on <FSTESTS_WORK_DIR>/scratch",
            MODULE.normalize_message("FITRIM not supported on /tmp/swordfs-fstests.ZYlNPG/scratch"),
        )
        self.assertEqual(
            "unrelated /tmp/example.ABC123/scratch path",
            MODULE.normalize_message("unrelated /tmp/example.ABC123/scratch path"),
        )

    def test_message_normalization_canonicalizes_fstests_result_root_but_keeps_test_file(self):
        self.assertEqual(
            "- output mismatch (see <FSTESTS_RESULT_DIR>/generic/006.out.bad)",
            MODULE.normalize_message(
                "- output mismatch (see /home/runner/work/SwordFS/SwordFS/build/fstests-conformance/raw/results/generic/006.out.bad)"
            ),
        )

    def test_classifies_supported_known_gap_xpass_and_explicit_not_applicable(self):
        selected = {"generic/001", "generic/002", "generic/003", "generic/004"}
        actual = {
            "generic/001": MODULE.RawResult("generic/001", "PASS"),
            "generic/002": MODULE.RawResult("generic/002", "FAIL", "mismatch"),
            "generic/003": MODULE.RawResult("generic/003", "PASS"),
            "generic/004": MODULE.RawResult("generic/004", "NOTRUN", "requires local block device"),
        }
        gaps = {
            "generic/002": MODULE.Gap("known_semantic_defect", "#42", "FAIL", "mismatch", "fsync semantics"),
            "generic/003": MODULE.Gap("known_unsupported", "#43", "FAIL", "missing fallocate", "fallocate missing"),
            "generic/004": MODULE.Gap(
                "upstream_not_applicable", "-", "NOTRUN", "requires local block device", "FUSE has no block device"
            ),
        }
        results = MODULE.classify(selected, actual, {"generic/001"}, gaps)
        by_test = {result.test: result.classification for result in results}
        self.assertEqual("PASS", by_test["generic/001"])
        self.assertEqual("KNOWN_SEMANTIC_DEFECT", by_test["generic/002"])
        self.assertEqual("XPASS", by_test["generic/003"])
        self.assertEqual("UPSTREAM_NOT_APPLICABLE", by_test["generic/004"])

    def test_unclassified_notrun_is_not_automatically_not_applicable(self):
        selected = {"generic/100"}
        actual = {"generic/100": MODULE.RawResult("generic/100", "NOTRUN", "fallocate not supported")}
        result = MODULE.classify(selected, actual, set(), {})[0]
        self.assertEqual("UNCLASSIFIED_NOTRUN", result.classification)

    def test_known_unsupported_notrun_requires_issue_and_exact_reason(self):
        selected = {"generic/101"}
        gap = MODULE.Gap("known_unsupported", "#77", "NOTRUN", "requires fallocate", "fallocate pending")
        actual = {"generic/101": MODULE.RawResult("generic/101", "NOTRUN", "different reason")}
        result = MODULE.classify(selected, actual, set(), {"generic/101": gap})[0]
        self.assertEqual("BASELINE_REASON_MISMATCH", result.classification)

    def test_missing_selected_result_is_infrastructure(self):
        selected = {"generic/001", "generic/002"}
        actual = {"generic/001": MODULE.RawResult("generic/001", "PASS")}
        results = MODULE.classify(selected, actual, {"generic/001", "generic/002"}, {})
        by_test = {result.test: result.classification for result in results}
        self.assertEqual("INFRASTRUCTURE", by_test["generic/002"])

    def test_explicit_ci_deferred_test_is_not_treated_as_missing_infrastructure(self):
        selected = {"generic/001", "generic/069"}
        actual = {"generic/001": MODULE.RawResult("generic/001", "PASS")}
        results = MODULE.classify(
            selected,
            actual,
            {"generic/001"},
            {},
            {"generic/069": "too slow for the PR FUSE gate"},
        )
        by_test = {result.test: result for result in results}
        self.assertEqual("DEFERRED_CI", by_test["generic/069"].classification)
        self.assertEqual("too slow for the PR FUSE gate", by_test["generic/069"].reason)

    def test_deferred_manifest_is_exact_and_requires_reason(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "deferred.tsv"
            path.write_text(
                "test\treason\n"
                "generic/069\tFUSE runtime is CI-prohibitive\n",
                encoding="utf-8",
            )
            self.assertEqual(
                {"generic/069": "FUSE runtime is CI-prohibitive"},
                MODULE.load_deferred(path),
            )

    def test_deferred_test_cannot_also_be_supported_or_known_gap(self):
        with self.assertRaises(MODULE.BaselineError):
            MODULE.classify(
                {"generic/069"},
                {},
                {"generic/069"},
                {},
                {"generic/069": "temporarily deferred"},
            )

    def test_deferred_test_outside_selected_population_blocks(self):
        results = MODULE.classify(
            {"generic/001"},
            {"generic/001": MODULE.RawResult("generic/001", "PASS")},
            {"generic/001"},
            {},
            {"generic/069": "temporarily deferred"},
        )
        by_test = {result.test: result.classification for result in results}
        self.assertEqual("BASELINE_NOT_SELECTED", by_test["generic/069"])

    def test_summary_reports_deferred_as_selected_but_not_executed_or_supported(self):
        selected = {"generic/001", "generic/069"}
        supported = {"generic/001"}
        deferred = {"generic/069": "temporarily deferred"}
        observations = MODULE.classify(
            selected,
            {"generic/001": MODULE.RawResult("generic/001", "PASS")},
            supported,
            {},
            deferred,
        )
        summary = MODULE.summarize(observations, selected, supported, {}, deferred)
        self.assertEqual(2, summary["selected_count"])
        self.assertEqual(1, summary["executed_count"])
        self.assertEqual(1, summary["deferred_count"])
        self.assertEqual(1, summary["supported_count"])
        self.assertEqual(100.0, summary["executed_classified_percent"])
        self.assertEqual(50.0, summary["classified_percent"])
        self.assertEqual(50.0, summary["overall_support_percent"])
        self.assertEqual(0, summary["blocking_count"])

    def test_summary_counts_observed_execution_instead_of_assuming_non_deferred_population(self):
        selected = {"generic/001", "generic/002", "generic/069"}
        supported = {"generic/001", "generic/002"}
        deferred = {"generic/069": "temporarily deferred"}
        observations = MODULE.classify(
            selected,
            {"generic/001": MODULE.RawResult("generic/001", "PASS")},
            supported,
            {},
            deferred,
        )
        summary = MODULE.summarize(observations, selected, supported, {}, deferred)
        self.assertEqual(1, summary["executed_count"])
        self.assertEqual(100.0, summary["executed_classified_percent"])
        self.assertEqual(1, summary["blocking_count"])

    def test_unexpected_execution_of_deferred_case_is_counted_and_blocks(self):
        selected = {"generic/001", "generic/069"}
        supported = {"generic/001"}
        deferred = {"generic/069": "temporarily deferred"}
        observations = MODULE.classify(
            selected,
            {
                "generic/001": MODULE.RawResult("generic/001", "PASS"),
                "generic/069": MODULE.RawResult("generic/069", "PASS"),
            },
            supported,
            {},
            deferred,
        )
        summary = MODULE.summarize(observations, selected, supported, {}, deferred)
        self.assertEqual(2, summary["executed_count"])
        self.assertEqual(50.0, summary["executed_classified_percent"])
        self.assertEqual(1, summary["blocking_count"])

    def test_unexpected_observed_test_is_infrastructure(self):
        selected = {"generic/001"}
        actual = {
            "generic/001": MODULE.RawResult("generic/001", "PASS"),
            "generic/002": MODULE.RawResult("generic/002", "PASS"),
        }
        results = MODULE.classify(selected, actual, {"generic/001"}, {})
        by_test = {result.test: result.classification for result in results}
        self.assertEqual("INFRASTRUCTURE", by_test["generic/002"])

    def test_supported_test_becoming_notrun_blocks(self):
        selected = {"generic/001"}
        actual = {"generic/001": MODULE.RawResult("generic/001", "NOTRUN", "requires a capability")}
        result = MODULE.classify(selected, actual, {"generic/001"}, {})[0]
        self.assertEqual("BASELINE_NOT_APPLICABLE", result.classification)

    def test_known_failure_becoming_notrun_requires_reclassification(self):
        selected = {"generic/001"}
        actual = {"generic/001": MODULE.RawResult("generic/001", "NOTRUN", "different prerequisite")}
        gap = MODULE.Gap("known_semantic_defect", "#42", "FAIL", "known failure output", "known failure")
        result = MODULE.classify(selected, actual, set(), {"generic/001": gap})[0]
        self.assertEqual("BASELINE_RESULT_MISMATCH", result.classification)

    def test_supported_and_gap_baselines_must_be_disjoint(self):
        with self.assertRaises(MODULE.BaselineError):
            MODULE.classify(
                {"generic/001"},
                {"generic/001": MODULE.RawResult("generic/001", "PASS")},
                {"generic/001"},
                {"generic/001": MODULE.Gap("known_unsupported", "#42", "FAIL", "missing capability", "missing")},
            )

    def test_baseline_test_outside_selected_population_blocks(self):
        results = MODULE.classify(
            {"generic/001"},
            {"generic/001": MODULE.RawResult("generic/001", "PASS")},
            {"generic/001", "generic/999"},
            {},
        )
        by_test = {result.test: result.classification for result in results}
        self.assertEqual("BASELINE_NOT_SELECTED", by_test["generic/999"])

    def test_selected_manifest_ignores_metadata_comments_and_rejects_duplicates(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "selected.txt"
            path.write_text(
                "# fstests_commit=abc\n# fstests_group=generic/quick\ngeneric/001\ngeneric/002\n",
                encoding="utf-8",
            )
            self.assertEqual({"generic/001", "generic/002"}, MODULE.load_selected(path))
            path.write_text("generic/001\ngeneric/001\n", encoding="utf-8")
            with self.assertRaises(MODULE.BaselineError):
                MODULE.load_selected(path)

    def test_report_distinguishes_overall_support_from_supported_gate(self):
        selected = {"generic/001", "generic/002", "generic/003", "generic/004"}
        supported = {"generic/001", "generic/002"}
        observations = MODULE.classify(
            selected,
            {
                "generic/001": MODULE.RawResult("generic/001", "PASS"),
                "generic/002": MODULE.RawResult("generic/002", "PASS"),
                "generic/003": MODULE.RawResult("generic/003", "NOTRUN", "not applicable"),
                "generic/004": MODULE.RawResult("generic/004", "NOTRUN", "not applicable"),
            },
            supported,
            {
                "generic/003": MODULE.Gap("upstream_not_applicable", "-", "NOTRUN", "not applicable", "n/a"),
                "generic/004": MODULE.Gap("upstream_not_applicable", "-", "NOTRUN", "not applicable", "n/a"),
            },
            {},
        )
        summary = MODULE.summarize(
            observations,
            selected,
            supported,
            {
                "generic/003": MODULE.Gap("upstream_not_applicable", "-", "NOTRUN", "not applicable", "n/a"),
                "generic/004": MODULE.Gap("upstream_not_applicable", "-", "NOTRUN", "not applicable", "n/a"),
            },
            {},
        )
        payload = {"metadata": {}, "summary": summary}
        report = MODULE.render_markdown(payload, observations)
        self.assertEqual(50.0, summary["overall_support_percent"])
        self.assertEqual(100.0, summary["supported_gate_percent"])
        self.assertIn("| Overall support | 50.00% |", report)
        self.assertIn("| Supported gate | 100.00% |", report)

    def test_supported_set_is_monotonic_without_explicit_override(self):
        with self.assertRaises(MODULE.BaselineError):
            MODULE.validate_supported_monotonicity({"generic/001"}, {"generic/001", "generic/002"}, False)
        MODULE.validate_supported_monotonicity({"generic/001"}, {"generic/001", "generic/002"}, True)

    def test_deferred_set_cannot_grow_without_explicit_override(self):
        previous = {"generic/069": "existing slow test"}
        current = {
            "generic/069": "existing slow test",
            "generic/471": "newly deferred slow test",
        }
        with self.assertRaises(MODULE.BaselineError):
            MODULE.validate_deferred_growth(current, previous, False)
        MODULE.validate_deferred_growth(current, previous, True)

    def test_deferred_set_may_shrink_without_override(self):
        MODULE.validate_deferred_growth({}, {"generic/069": "restore to execution"}, False)

    def test_gap_manifest_requires_issue_for_semantic_gap(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "test\tcategory\tissue\texpected_result\texpected_message\treason\n"
                "generic/001\tknown_unsupported\t-\tFAIL\t-\tmissing capability\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.BaselineError):
                MODULE.load_gaps(path)

    def test_notrun_manifest_requires_preserved_skip_reason(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "test\tcategory\tissue\texpected_result\texpected_message\treason\n"
                "generic/001\tupstream_not_applicable\t-\tNOTRUN\t\tlocal block-device assumption\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.BaselineError):
                MODULE.load_gaps(path)

    def test_not_applicable_failure_can_remain_in_selected_population(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "test\tcategory\tissue\texpected_result\texpected_message\treason\n"
                "generic/001\tupstream_not_applicable\t-\tFAIL\toutput mismatch\trequires local block-device remount semantics\n",
                encoding="utf-8",
            )
            gaps = MODULE.load_gaps(path)
            result = MODULE.classify(
                {"generic/001"},
                {"generic/001": MODULE.RawResult("generic/001", "FAIL", "output mismatch")},
                set(),
                gaps,
            )[0]
            self.assertEqual("UPSTREAM_NOT_APPLICABLE", result.classification)

    def test_environment_failure_can_be_recorded_without_pruning_test(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "test\tcategory\tissue\texpected_result\texpected_message\treason\n"
                "generic/749\tenvironment\t-\tFAIL\tdumped core\thost systemd coredump policy changes expected SIGBUS handling\n",
                encoding="utf-8",
            )
            gaps = MODULE.load_gaps(path)
            result = MODULE.classify(
                {"generic/749"},
                {"generic/749": MODULE.RawResult("generic/749", "FAIL", "dumped core")},
                set(),
                gaps,
            )[0]
            self.assertEqual("ENVIRONMENT", result.classification)

    def test_nonsemantic_failure_reason_mismatch_blocks(self):
        gap = MODULE.Gap(
            "environment", "-", "FAIL", "dumped core", "host coredump policy"
        )
        result = MODULE.classify(
            {"generic/749"},
            {"generic/749": MODULE.RawResult("generic/749", "FAIL", "output mismatch")},
            set(),
            {"generic/749": gap},
        )[0]
        self.assertEqual("BASELINE_REASON_MISMATCH", result.classification)

    def test_nonsemantic_failure_requires_exact_failure_message(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "test\tcategory\tissue\texpected_result\texpected_message\treason\n"
                "generic/749\tenvironment\t-\tFAIL\t-\thost coredump policy\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.BaselineError):
                MODULE.load_gaps(path)

    def test_semantic_failure_requires_exact_failure_message(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "test\tcategory\tissue\texpected_result\texpected_message\treason\n"
                "generic/001\tknown_semantic_defect\t#42\tFAIL\t-\tknown fsync defect\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.BaselineError):
                MODULE.load_gaps(path)

    def test_semantic_failure_message_change_blocks(self):
        gap = MODULE.Gap(
            "known_semantic_defect", "#42", "FAIL", "known output mismatch", "known fsync defect"
        )
        result = MODULE.classify(
            {"generic/001"},
            {"generic/001": MODULE.RawResult("generic/001", "FAIL", "different output mismatch")},
            set(),
            {"generic/001": gap},
        )[0]
        self.assertEqual("BASELINE_REASON_MISMATCH", result.classification)

    def test_bootstrap_preserves_failure_message_for_manual_review(self):
        observations = [
            MODULE.Observation(
                "generic/001",
                "FAIL",
                "UNEXPECTED_FAIL",
                "output mismatch for known path",
            )
        ]
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = pathlib.Path(temporary)
            MODULE.write_bootstrap(output_dir, observations)
            rows = (output_dir / "bootstrap-gaps.tsv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(
                "generic/001\tTODO\t#TODO\tFAIL\toutput mismatch for known path\tclassify root cause",
                rows[1],
            )

    def test_xunit_loader_preserves_pass_fail_and_notrun(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "result.xml"
            _xunit(
                path,
                [
                    ("generic/001", "PASS", ""),
                    ("generic/002", "FAIL", " output mismatch "),
                    ("generic/003", "NOTRUN", " requires xattr "),
                ],
            )
            results = MODULE.load_xunit(path)
            self.assertEqual("PASS", results["generic/001"].result)
            self.assertEqual("output mismatch", results["generic/002"].message)
            self.assertEqual("requires xattr", results["generic/003"].message)

    def test_missing_xunit_is_reportable_as_infrastructure(self):
        with tempfile.TemporaryDirectory() as temporary:
            missing = pathlib.Path(temporary) / "missing.xml"
            results, error = MODULE.try_load_xunit(missing, "result XUnit")
            self.assertEqual({}, results)
            self.assertIsNotNone(error)
            self.assertIn("result XUnit", error)


if __name__ == "__main__":
    unittest.main()
