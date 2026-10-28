#!/usr/bin/env python3
"""Ensure failed-only reruns verify each CI artifact's producing attempt."""

from __future__ import annotations

import pathlib
import re
import unittest

CI = pathlib.Path(__file__).resolve().parents[2] / ".github/workflows/ci.yml"
JOBS = ("build-release-core", "build-fstests-tools")


def job_sections(content: str) -> dict[str, str]:
    boundaries = list(re.finditer(r"^  ([a-z][a-z-]*):\s*$", content, re.MULTILINE))
    return {
        boundary.group(1): content[boundary.end():boundaries[index + 1].start() if index + 1 < len(boundaries) else None]
        for index, boundary in enumerate(boundaries)
    }


class ArtifactProducerAttemptTest(unittest.TestCase):
    def test_producers_export_hash_and_actual_attempt(self) -> None:
        sections = job_sections(CI.read_text())
        for name in JOBS:
            with self.subTest(job=name):
                job = sections[name]
                self.assertIn("archive_sha: ${{ steps.package.outputs.archive_sha }}", job)
                self.assertIn("archive_attempt: ${{ steps.package.outputs.archive_attempt }}", job)
                self.assertIn('echo "archive_attempt=${GITHUB_RUN_ATTEMPT}" >> "${GITHUB_OUTPUT}"', job)

    def test_consumers_verify_producer_attempt_not_consumer_retry(self) -> None:
        sections = job_sections(CI.read_text())
        consumers = ("pjdfstest-conformance", "fstests-conformance-shard")
        for name in consumers:
            job = sections[name]
            self.assertIn("--run-attempt \"${{ needs.build-release-core.outputs.archive_attempt }}\"", job)
            self.assertNotRegex(job, r'--run-attempt "\$\{GITHUB_RUN_ATTEMPT\}"')
            if name != "pjdfstest-conformance":
                self.assertIn("--run-attempt \"${{ needs.build-fstests-tools.outputs.archive_attempt }}\"", job)

    def test_attempts_preserved_as_attested_artifact_metadata(self) -> None:
        for name in ("release_artifact.py", "fstests_tools_artifact.py"):
            content = (CI.parents[2] / "scripts/ci" / name).read_text()
            self.assertIn('"run_attempt": args.run_attempt', content)


if __name__ == "__main__":
    unittest.main()
