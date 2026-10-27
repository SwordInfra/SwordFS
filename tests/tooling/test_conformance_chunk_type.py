#!/usr/bin/env python3
"""Validate opt-in POSIX format selection without invoking FUSE or Redis."""

from __future__ import annotations

import os
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = ROOT / "scripts/conformance/chunk-type.sh"


class ConformanceChunkTypeTest(unittest.TestCase):
    @staticmethod
    def inspect(value: str | None) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        env.pop("SWORDFS_CONFORMANCE_CHUNK_TYPE", None)
        if value is not None:
            env["SWORDFS_CONFORMANCE_CHUNK_TYPE"] = value
        return subprocess.run(
            [
                "bash",
                "-euc",
                'source "$1"; printf "type=%s\\n" "$CONFORMANCE_CHUNK_TYPE"; printf "arg=%s\\n" "${CONFORMANCE_FORMAT_CHUNK_ARGS[@]}"',
                "--",
                str(HELPER),
            ],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def test_default_and_explicit_cow_do_not_append_format_flags(self) -> None:
        for value in (None, "cow", ""):
            with self.subTest(value=value):
                result = self.inspect(value)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, "type=cow\narg=\n")

    def test_slice_adds_exactly_one_format_mechanism(self) -> None:
        result = self.inspect("chunk_slice")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "type=chunk_slice\narg=--chunk-type\narg=chunk_slice\n")

    def test_unknown_and_shell_metacharacters_fail_closed(self) -> None:
        for value in ("redis_cache", "slice", "chunk_slice; exit 0", "cow --bucket evil"):
            with self.subTest(value=value):
                result = self.inspect(value)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(result.stdout, "")
                self.assertIn("unsupported conformance chunk type", result.stderr)


if __name__ == "__main__":
    unittest.main()
