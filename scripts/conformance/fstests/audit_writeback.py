#!/usr/bin/env python3
"""Fail closed on unexpected SwordFS writeback errors in one fstests case.

Take byte offsets just before the case runs, after which the harness unmounts
both FUSE volumes. Do not infer success from the upstream XUnit exit code:
several workloads ignore asynchronous pwrite/RELEASE failures.
"""
from __future__ import annotations

import argparse
import pathlib
import re

FATAL = ("FileReadWriter::Flush chunk FAILED", "local RELEASE failed")
TEST_RE = re.compile(r"^[a-z0-9_-]+/[0-9]+$")
# generic/079 deliberately exercises immutable/append-only enforcement and
# expects the backend to reject dirty buffers after the inode policy changes.
INTENTIONAL_POLICY_REJECTION = "finalize chunk write rejected by inode policy"


def audit(test: str, logs: dict[str, tuple[pathlib.Path, int]]) -> list[str]:
    if not TEST_RE.fullmatch(test):
        raise ValueError(f"invalid testcase identity: {test!r}")
    errors: list[str] = []
    for role, (path, start) in logs.items():
        if role not in {"test", "scratch"} or start < 0:
            raise ValueError(f"invalid daemon log identity or offset: {role} {start}")
        with path.open("rb") as stream:
            length = stream.seek(0, 2)
            if length < start:
                raise ValueError(f"{role} daemon log shrank while running {test}")
            stream.seek(start)
            fragment = stream.read().decode("utf-8", errors="replace")
        for line in fragment.splitlines():
            if not any(marker in line for marker in FATAL):
                continue
            if test == "generic/079" and INTENTIONAL_POLICY_REJECTION in line:
                continue
            errors.append(f"{role}: {line}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--test", required=True)
    parser.add_argument("--test-log", type=pathlib.Path, required=True)
    parser.add_argument("--test-start", type=int, required=True)
    parser.add_argument("--scratch-log", type=pathlib.Path, required=True)
    parser.add_argument("--scratch-start", type=int, required=True)
    parser.add_argument("--evidence", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        errors = audit(args.test, {"test": (args.test_log, args.test_start),
                                   "scratch": (args.scratch_log, args.scratch_start)})
    except (OSError, ValueError) as exc:
        errors = [f"unable to audit writeback evidence: {exc}"]
    if errors:
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text("\n".join(errors) + "\n", encoding="utf-8")
        print(f"ERROR: {args.test}: {len(errors)} unexpected SwordFS writeback failure(s)")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
