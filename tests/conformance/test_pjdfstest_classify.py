import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "pjdfstest_classify.py"
ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("pjdfstest_classify", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC and SPEC.loader
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class PjdfstestClassifyTest(unittest.TestCase):
    def _tap(self, root: pathlib.Path, name: str, content: str) -> pathlib.Path:
        path = root / f"tests/{name}.t.tap"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
        return path

    def test_classifies_supported_gap_regression_xpass_and_upstream_skip(self):
        with tempfile.TemporaryDirectory() as temporary:
            raw = pathlib.Path(temporary)
            self._tap(
                raw,
                "open/00",
                "\n".join(
                    [
                        "1..5",
                        "ok 1",
                        "not ok 2 - expected 0, got EIO",
                        "not ok 3 - regression",
                        "ok 4",
                        "not ok 5 # TODO Linux upstream difference",
                    ]
                ),
            )
            observations, infrastructure = MODULE.collect_observations(raw)
            gaps = {
                "tests/open/00.t#2": MODULE.Gap("known_semantic_defect", "#42", "known open defect"),
                "tests/open/00.t#4": MODULE.Gap("known_unsupported", "#43", "feature pending"),
            }
            results = MODULE.classify(
                observations,
                infrastructure,
                {"tests/open/00.t#1", "tests/open/00.t#3"},
                gaps,
            )
            by_id = {result.assertion_id: result.classification for result in results}
            self.assertEqual("PASS", by_id["tests/open/00.t#1"])
            self.assertEqual("KNOWN_SEMANTIC_DEFECT", by_id["tests/open/00.t#2"])
            self.assertEqual("REGRESSION", by_id["tests/open/00.t#3"])
            self.assertEqual("XPASS", by_id["tests/open/00.t#4"])
            self.assertEqual("UPSTREAM_NOT_APPLICABLE", by_id["tests/open/00.t#5"])

    def test_unclassified_results_and_incomplete_tap_block(self):
        with tempfile.TemporaryDirectory() as temporary:
            raw = pathlib.Path(temporary)
            self._tap(raw, "mkdir/00", "1..3\nok 1\nnot ok 2 - failure\n")
            observations, infrastructure = MODULE.collect_observations(raw)
            results = MODULE.classify(observations, infrastructure, set(), {})
            classifications = {result.classification for result in results}
            self.assertIn("UNCLASSIFIED_PASS", classifications)
            self.assertIn("UNEXPECTED_FAIL", classifications)
            self.assertIn("INFRASTRUCTURE", classifications)
            summary = MODULE.summarize(results, set())
            self.assertEqual(3, summary["blocking_count"])
            self.assertEqual(50.0, summary["overall_support_percent"])

    def test_nonzero_script_exit_is_infrastructure_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            raw = pathlib.Path(temporary)
            tap = self._tap(raw, "mkdir/00", "1..1\nok 1\n")
            tap.with_suffix(".exit").write_text("124\n", encoding="utf-8")
            observations, infrastructure = MODULE.collect_observations(raw)
            results = MODULE.classify(observations, infrastructure, {"tests/mkdir/00.t#1"}, {})
            by_id = {result.assertion_id: result for result in results}
            self.assertEqual("INFRASTRUCTURE", by_id["tests/mkdir/00.t#exit"].classification)
            self.assertIn("124", by_id["tests/mkdir/00.t#exit"].message)

    def test_whole_test_skip_does_not_hide_nonzero_script_exit(self):
        with tempfile.TemporaryDirectory() as temporary:
            raw = pathlib.Path(temporary)
            tap = self._tap(raw, "open/24", "1..0 # SKIP unsupported OS\n")
            tap.with_suffix(".exit").write_text("2\n", encoding="utf-8")
            observations, infrastructure = MODULE.collect_observations(raw)
            self.assertEqual([], observations)
            self.assertEqual(1, len(infrastructure))
            self.assertEqual("INFRASTRUCTURE", infrastructure[0].classification)

    def test_repository_baseline_is_parseable_and_disjoint(self):
        supported = MODULE.load_supported(ROOT / "conformance/pjdfstest/supported.txt")
        gaps = MODULE.load_gaps(ROOT / "conformance/pjdfstest/known-gaps.tsv")
        self.assertTrue(supported)
        self.assertFalse(supported & gaps.keys())
        # The pinned upstream revision has 8,770 applicable assertions. Moving a
        # known gap into the supported set must preserve complete classification;
        # a fully supported baseline may legitimately have no known gaps.
        self.assertEqual(8770, len(supported | gaps.keys()))

    def test_baseline_assertion_cannot_silently_become_upstream_todo(self):
        with tempfile.TemporaryDirectory() as temporary:
            raw = pathlib.Path(temporary)
            self._tap(raw, "open/00", "1..1\nok 1 # TODO now upstream-specific\n")
            observations, infrastructure = MODULE.collect_observations(raw)
            results = MODULE.classify(observations, infrastructure, {"tests/open/00.t#1"}, {})
            self.assertEqual("BASELINE_NOT_APPLICABLE", results[0].classification)

    def test_supported_set_removal_requires_explicit_override(self):
        previous = {"tests/open/00.t#1", "tests/open/00.t#2"}
        current = {"tests/open/00.t#1"}
        with self.assertRaises(MODULE.BaselineError):
            MODULE.validate_supported_monotonicity(current, previous, False)
        MODULE.validate_supported_monotonicity(current, previous, True)

    def test_baseline_not_observed_blocks(self):
        with tempfile.TemporaryDirectory() as temporary:
            raw = pathlib.Path(temporary)
            self._tap(raw, "unlink/00", "1..1\nok 1\n")
            observations, infrastructure = MODULE.collect_observations(raw)
            results = MODULE.classify(
                observations,
                infrastructure,
                {"tests/unlink/00.t#1", "tests/unlink/00.t#2"},
                {},
            )
            by_id = {result.assertion_id: result.classification for result in results}
            self.assertEqual("BASELINE_NOT_OBSERVED", by_id["tests/unlink/00.t#2"])

    def test_gap_manifest_requires_issue_for_semantic_gap(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "selector\tcategory\tissue\treason\n"
                "tests/open/00.t#1\tknown_unsupported\t-\tnot supported\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.BaselineError):
                MODULE.load_gaps(path)

    def test_assertion_selectors_expand_and_compact_without_wildcards(self):
        selector = "tests/open/00.t#1-3,5,8-9"
        assertion_ids = MODULE.expand_assertion_selector(selector)
        self.assertEqual(
            [
                "tests/open/00.t#1",
                "tests/open/00.t#2",
                "tests/open/00.t#3",
                "tests/open/00.t#5",
                "tests/open/00.t#8",
                "tests/open/00.t#9",
            ],
            assertion_ids,
        )
        self.assertEqual([selector], MODULE.compact_assertion_ids(assertion_ids))
        with self.assertRaises(MODULE.BaselineError):
            MODULE.expand_assertion_selector("tests/open/*.t#1")

    def test_gap_manifest_requires_github_issue_number_shape(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "gaps.tsv"
            path.write_text(
                "selector\tcategory\tissue\treason\n"
                "tests/open/00.t#1\tknown_semantic_defect\tissue-42\twrong shape\n",
                encoding="utf-8",
            )
            with self.assertRaises(MODULE.BaselineError):
                MODULE.load_gaps(path)

    def test_rendered_payload_is_json_serializable(self):
        observation = MODULE.Observation(
            assertion_id="tests/open/00.t#1",
            test_path="tests/open/00.t",
            category="open",
            number=1,
            result="pass",
            classification="PASS",
        )
        summary = MODULE.summarize([observation], {observation.assertion_id})
        payload = {"metadata": {}, "summary": summary, "tests": [observation.as_dict()]}
        json.dumps(payload)

    def test_known_gap_report_summarizes_issue_links(self):
        observation = MODULE.Observation(
            assertion_id="tests/open/00.t#1",
            test_path="tests/open/00.t",
            category="open",
            number=1,
            result="fail",
            classification="KNOWN_SEMANTIC_DEFECT",
            issue="#210",
            reason="permission semantics",
        )
        payload = {
            "metadata": {},
            "summary": MODULE.summarize([observation], set()),
            "tests": [observation.as_dict()],
        }
        report = MODULE.render_markdown(payload, [observation])
        self.assertIn("## Known-gap Issues", report)
        self.assertIn("issues/210", report)


if __name__ == "__main__":
    unittest.main()
