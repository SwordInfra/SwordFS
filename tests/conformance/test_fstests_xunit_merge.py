import importlib.util
import pathlib
import tempfile
import unittest
import xml.etree.ElementTree as ET


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "fstests_xunit_merge.py"
SPEC = importlib.util.spec_from_file_location("fstests_xunit_merge", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def write_part(path: pathlib.Path, test: str, result: str = "pass") -> None:
    suite = ET.Element("testsuite", name="xfstests", tests="1")
    case = ET.SubElement(suite, "testcase", classname="xfstests.global", name=test, time="1")
    if result == "notrun":
        ET.SubElement(case, "skipped", message="not applicable")
    elif result == "fail":
        ET.SubElement(case, "failure", message="output mismatch", type="TestFail")
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


class FstestsXunitMergeTest(unittest.TestCase):
    def test_merges_one_test_reports(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            parts = root / "parts"
            parts.mkdir()
            write_part(parts / "generic-002.xml", "generic/002", "fail")
            write_part(parts / "generic-001.xml", "generic/001")
            output = root / "result.xml"

            MODULE.merge(parts, output)

            cases = list(ET.parse(output).getroot().iter("testcase"))
            names = [case.get("name") for case in cases]
            self.assertEqual(["generic/001", "generic/002"], names)
            self.assertEqual([], list(cases[0]))
            self.assertEqual("failure", list(cases[1])[0].tag)
            self.assertEqual("output mismatch", list(cases[1])[0].get("message"))


    def test_preserves_notrun_result(self):
        with tempfile.TemporaryDirectory() as temporary:
            parts = pathlib.Path(temporary) / "parts"
            parts.mkdir()
            write_part(parts / "generic-003.xml", "generic/003", "notrun")
            output = pathlib.Path(temporary) / "result.xml"

            MODULE.merge(parts, output)

            case = next(ET.parse(output).getroot().iter("testcase"))
            skipped = list(case)
            self.assertEqual(1, len(skipped))
            self.assertEqual("skipped", skipped[0].tag)
            self.assertEqual("not applicable", skipped[0].get("message"))

    def test_rejects_duplicate_testcase_parts(self):
        with tempfile.TemporaryDirectory() as temporary:
            parts = pathlib.Path(temporary)
            write_part(parts / "a.xml", "generic/001")
            write_part(parts / "b.xml", "generic/001")
            with self.assertRaises(MODULE.MergeError):
                MODULE.merge(parts, parts / "result.xml")

    def test_rejects_part_with_multiple_testcases(self):
        with tempfile.TemporaryDirectory() as temporary:
            parts = pathlib.Path(temporary)
            suite = ET.Element("testsuite")
            ET.SubElement(suite, "testcase", name="generic/001")
            ET.SubElement(suite, "testcase", name="generic/002")
            ET.ElementTree(suite).write(parts / "bad.xml", encoding="utf-8")
            with self.assertRaises(MODULE.MergeError):
                MODULE.merge(parts, parts / "result.xml")


if __name__ == "__main__":
    unittest.main()
