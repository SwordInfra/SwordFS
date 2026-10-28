#!/usr/bin/env python3
"""Guard mechanism-qualified fstests evidence against mislabeled shard volumes."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/conformance/fstests/aggregate.py"
SPEC = importlib.util.spec_from_file_location("fstests_aggregate", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class FstestsAggregateMechanismTest(unittest.TestCase):
    def test_expected_slice_requires_slice_metadata_from_every_shard(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            shard = root / "shards" / "fstests-conformance-supported-0"
            (shard / "raw/results").mkdir(parents=True)
            (shard / "shard.json").write_text(
                json.dumps({"shard": "supported-0", "planned_tests": ["generic/001"]})
            )
            (shard / "harness-status.txt").write_text("0\n")
            (shard / "raw/results/result.xml").write_text(
                '<testsuites><testsuite tests="1"><testcase name="generic/001"/></testsuite></testsuites>'
            )
            metadata = shard / "environment.json"
            for value, should_pass in (("chunk_slice", True), ("cow", False), (None, False)):
                with self.subTest(value=value):
                    if value is None:
                        metadata.unlink(missing_ok=True)
                    else:
                        metadata.write_text(json.dumps({"chunk_type": value}))
                    output = root / "out"
                    errors = MODULE.aggregate(
                        root / "shards", output, {"supported-0": ["generic/001"]}, ["generic/001"],
                        set(), expected_chunk_type="chunk_slice"
                    )
                    self.assertEqual(errors == [], should_pass, errors)
                    self.assertEqual((output / "harness-status.txt").read_text(), "0\n" if should_pass else "2\n")
                    if should_pass:
                        self.assertEqual(json.loads((output / "environment.json").read_text())["chunk_type"], "chunk_slice")


if __name__ == "__main__":
    unittest.main()
