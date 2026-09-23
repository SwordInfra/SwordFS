import importlib.util
import json
import pathlib
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "fstests_aggregate.py"
SPEC = importlib.util.spec_from_file_location("fstests_aggregate", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def write_artifact(root: pathlib.Path, name: str, planned: list[str], actual: list[str], status: int = 0) -> None:
    artifact = root / f"fstests-conformance-{name}"
    results = artifact / "raw/results"
    results.mkdir(parents=True)
    (artifact / "shard.json").write_text(
        json.dumps({"shard": name, "planned_tests": planned}), encoding="utf-8"
    )
    (artifact / "harness-status.txt").write_text(f"{status}\n", encoding="utf-8")
    (artifact / "environment.json").write_text(
        json.dumps({"kernel": "test", "libfuse": "test", "backend": "redis+minio-s3"}), encoding="utf-8"
    )
    suite = ET.Element("testsuite")
    for test in actual:
        ET.SubElement(suite, "testcase", name=test)
    ET.ElementTree(suite).write(results / "result.xml", encoding="utf-8", xml_declaration=True)


class FstestsAggregateTest(unittest.TestCase):
    def test_complete_shards_merge_exact_population(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            shards = root / "shards"
            output = root / "out"
            shards.mkdir()
            plan = {"a": ["generic/001"], "b": ["generic/002"]}
            write_artifact(shards, "a", plan["a"], plan["a"])
            write_artifact(shards, "b", plan["b"], plan["b"])

            errors = MODULE.aggregate(shards, output, plan, ["generic/001", "generic/002"], set())

            self.assertEqual([], errors)
            self.assertEqual("0", (output / "harness-status.txt").read_text().strip())
            names = {
                case.get("name")
                for case in ET.parse(output / "raw/results/result.xml").getroot().iter("testcase")
            }
            self.assertEqual({"generic/001", "generic/002"}, names)

    def test_missing_shard_is_blocking_infrastructure(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            shards = root / "shards"
            output = root / "out"
            shards.mkdir()
            plan = {"a": ["generic/001"], "b": ["generic/002"]}
            write_artifact(shards, "a", plan["a"], plan["a"])

            errors = MODULE.aggregate(shards, output, plan, ["generic/001", "generic/002"], set())

            self.assertTrue(any("missing shard artifacts" in error for error in errors))
            self.assertTrue(any("missing selected testcase" in error for error in errors))
            self.assertEqual("2", (output / "harness-status.txt").read_text().strip())

    def test_duplicate_testcase_across_shards_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            shards = root / "shards"
            output = root / "out"
            shards.mkdir()
            plan = {"a": ["generic/001"], "b": ["generic/002"]}
            write_artifact(shards, "a", plan["a"], ["generic/001"])
            write_artifact(shards, "b", plan["b"], ["generic/001", "generic/002"])

            errors = MODULE.aggregate(shards, output, plan, ["generic/001", "generic/002"], set())

            self.assertTrue(any("duplicate testcase result across shards" in error for error in errors))
            self.assertEqual("2", (output / "harness-status.txt").read_text().strip())

    def test_deferred_testcase_must_not_appear_in_execution_results(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            shards = root / "shards"
            output = root / "out"
            shards.mkdir()
            plan = {"a": ["generic/001"]}
            write_artifact(shards, "a", plan["a"], ["generic/001", "generic/069"])

            errors = MODULE.aggregate(
                shards,
                output,
                plan,
                ["generic/001", "generic/069"],
                {"generic/069"},
            )

            self.assertTrue(any("deferred testcase appeared" in error for error in errors))
            self.assertEqual("2", (output / "harness-status.txt").read_text().strip())

    def test_nonzero_shard_harness_status_is_blocking(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            shards = root / "shards"
            output = root / "out"
            shards.mkdir()
            plan = {"a": ["generic/001"]}
            write_artifact(shards, "a", plan["a"], plan["a"], status=2)

            errors = MODULE.aggregate(shards, output, plan, ["generic/001"], set())

            self.assertTrue(any("harness exited with status 2" in error for error in errors))
            self.assertEqual("2", (output / "harness-status.txt").read_text().strip())

    def test_failed_matrix_result_is_blocking_even_with_complete_artifact(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            shards = root / "shards"
            output = root / "out"
            shards.mkdir()
            plan = {"a": ["generic/001"]}
            write_artifact(shards, "a", plan["a"], plan["a"])

            errors = MODULE.aggregate(
                shards,
                output,
                plan,
                ["generic/001"],
                set(),
                matrix_result="failure",
            )

            self.assertTrue(any("shard matrix completed with result 'failure'" in error for error in errors))
            self.assertEqual("2", (output / "harness-status.txt").read_text().strip())


if __name__ == "__main__":
    unittest.main()
