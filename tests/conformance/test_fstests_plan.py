import importlib.util
import pathlib
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "fstests_plan.py"
SPEC = importlib.util.spec_from_file_location("fstests_plan", MODULE_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class FstestsPlanTest(unittest.TestCase):
    def test_plan_partitions_selected_population_exactly_once(self):
        selected = [f"generic/{index:03d}" for index in range(1, 9)]
        supported = set(selected[:6])
        deferred = {selected[-1]}
        runtime = {
            selected[0]: 100,
            selected[1]: 80,
            selected[2]: 60,
            selected[3]: 40,
            selected[4]: 20,
            selected[5]: 10,
        }

        plan = MODULE.build_plan(selected, supported, deferred, runtime)

        planned = [test for shard in MODULE.SHARD_NAMES for test in plan[shard]]
        self.assertEqual(set(selected) - deferred, set(planned))
        self.assertEqual(len(planned), len(set(planned)))
        rest = {test for shard in MODULE.REST_SHARDS for test in plan[shard]}
        self.assertEqual({selected[6]}, rest)
        self.assertNotIn(selected[-1], planned)

    def test_supported_shards_use_runtime_balancing(self):
        selected = [f"generic/{index:03d}" for index in range(1, 5)]
        runtime = dict(zip(selected, [100, 90, 80, 70]))

        plan = MODULE.build_plan(selected, set(selected), set(), runtime)

        for shard in MODULE.SUPPORTED_SHARDS:
            self.assertEqual(1, len(plan[shard]))
        for shard in MODULE.REST_SHARDS:
            self.assertEqual([], plan[shard])

    def test_rest_population_is_count_balanced_deterministically(self):
        selected = [f"generic/{index:03d}" for index in range(1, 8)]

        plan = MODULE.build_plan(selected, set(), set(), {})

        self.assertEqual(
            ["generic/001", "generic/003", "generic/005", "generic/007"],
            plan["baseline-rest-0"],
        )
        self.assertEqual(
            ["generic/002", "generic/004", "generic/006"],
            plan["baseline-rest-1"],
        )

    def test_plan_rejects_deferred_case_outside_selection(self):
        with self.assertRaisesRegex(MODULE.PlanError, "deferred testcases are outside"):
            MODULE.build_plan(["generic/001"], {"generic/001"}, {"generic/002"}, {})

    def test_selection_identity_must_match_pinned_version(self):
        with self.assertRaisesRegex(MODULE.PlanError, "does not match pinned value"):
            MODULE.validate_selection_identity(
                {"fstests_commit": "old", "fstests_group": "generic/quick"},
                {"FSTESTS_COMMIT": "new", "FSTESTS_GROUP": "generic/quick"},
            )

    def test_verify_selection_xml_rejects_population_drift(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "result.xml"
            suite = ET.Element("testsuite")
            ET.SubElement(suite, "testcase", name="generic/001")
            ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)

            with self.assertRaisesRegex(MODULE.PlanError, "selection differs"):
                MODULE.verify_selection_xml(path, ["generic/001", "generic/002"])


if __name__ == "__main__":
    unittest.main()
