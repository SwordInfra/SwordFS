#!/usr/bin/env python3
"""Preserve existing CI skip policy while removing Slice experiment labels."""

from __future__ import annotations

from pathlib import Path
import re
import unittest

CI = Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml"


class CiSkipPolicyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = CI.read_text(encoding="utf-8")
        boundaries = list(re.finditer(r"^  ([a-z0-9-]+):\s*$", cls.workflow, re.MULTILINE))
        cls.jobs = {
            match.group(1): cls.workflow[match.end():boundaries[i + 1].start() if i + 1 < len(boundaries) else None]
            for i, match in enumerate(boundaries)
        }

    def test_documentation_only_pr_skips_native_ci(self) -> None:
        events = self.workflow.split("jobs:", 1)[0]
        self.assertIn("branches: [main]", events)
        self.assertIn("types: [opened, synchronize, reopened]", events)
        self.assertIn("workflow_dispatch:", events)
        self.assertNotIn("labeled", events)
        scope = self.jobs["change-scope"]
        self.assertIn("docs_only.py", scope)
        self.assertIn("run_full_ci", scope)
        self.assertIn("run_full_ci == 'false'", self.jobs["codecov-docs-only"])
        self.assertIn("run_full_ci == 'false'", self.jobs["build-and-test"])
        self.assertIn("run_full_ci == 'true'", self.jobs["fstests-conformance-shard"])
        self.assertIn("run_full_ci == 'true'", self.jobs["pjdfstest-conformance"])
        self.assertIn("run_full_ci=true", scope)
        self.assertNotIn("github.event.label", self.workflow)
        self.assertNotIn("github.event.pull_request.labels", self.workflow)

    def test_only_primary_conformance_jobs_remain(self) -> None:
        for job in ("e2e-test", "pjdfstest-conformance", "fstests-conformance-shard", "fstests-conformance"):
            self.assertIn(job, self.jobs)
        for old in ("slice-posix-experiment", "slice-346-regression"):
            self.assertNotIn(old, self.jobs)
        self.assertIn("FSTESTS_STRICT_WRITEBACK=1", self.jobs["fstests-conformance-shard"])
        self.assertIn("--expected-chunk-type chunk_slice", self.jobs["fstests-conformance"])
        self.assertIn("SWORDFS_E2E_CHUNK_TYPE=chunk_slice", self.jobs["e2e-test"])
        self.assertIn("SWORDFS_CONFORMANCE_CHUNK_TYPE=chunk_slice", self.jobs["pjdfstest-conformance"])

    def test_baseline_regressions_never_bypass_with_labels(self) -> None:
        for job in ("pjdfstest-conformance", "fstests-conformance"):
            self.assertIn("--previous-supported-file", self.jobs[job])
            self.assertNotIn("--allow-supported-removal", self.jobs[job])
        self.assertIn("--previous-deferred-file", self.jobs["fstests-conformance"])
        self.assertNotIn("--allow-deferred-addition", self.jobs["fstests-conformance"])
        self.assertNotIn("ALLOW_SUPPORTED_REMOVAL", self.workflow)
        self.assertNotIn("ALLOW_DEFERRED_ADDITION", self.workflow)


if __name__ == "__main__":
    unittest.main()
