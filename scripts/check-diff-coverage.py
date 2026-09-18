#!/usr/bin/env python3
"""Report incremental line/branch coverage for changed production code.

Line coverage is the repository-wide hard gate. Branch coverage is always
reported so reviewers can inspect untested business/state-machine decisions,
but a branch threshold is optional and should only be enabled when a caller
has deliberately scoped it to logic where a numeric branch gate is meaningful.
"""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import subprocess
import sys


HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@")


def ParseArgs() -> argparse.Namespace:
  parser = argparse.ArgumentParser()
  parser.add_argument("--base", required=True, help="Git base revision for the patch")
  parser.add_argument("--tracefile", required=True, help="LCOV tracefile")
  parser.add_argument("--source-prefix", default="src/", help="Production source path prefix")
  parser.add_argument("--line-threshold", type=float, default=90.0)
  parser.add_argument(
      "--branch-threshold",
      type=float,
      default=None,
      help="Optional hard branch-coverage gate. Omit for report-only branch coverage.",
  )
  return parser.parse_args()


def ChangedLines(base: str, source_prefix: str) -> dict[str, set[int]]:
  result = subprocess.run(
      [
          "git",
          "diff",
          "--unified=0",
          "--no-ext-diff",
          "--diff-filter=ACMR",
          f"{base}...HEAD",
          "--",
          source_prefix,
      ],
      check=True,
      capture_output=True,
      text=True,
  )

  changed: dict[str, set[int]] = collections.defaultdict(set)
  current_file: str | None = None
  for raw_line in result.stdout.splitlines():
    if raw_line.startswith("+++ "):
      path = raw_line[4:]
      if path == "/dev/null":
        current_file = None
      elif path.startswith("b/"):
        current_file = path[2:]
      else:
        current_file = path
      continue

    match = HUNK_RE.match(raw_line)
    if match and current_file is not None:
      start = int(match.group(1))
      count = int(match.group(2) or "1")
      changed[current_file].update(range(start, start + count))

  return dict(changed)


def NormalizeSource(path: str, repo_root: pathlib.Path) -> str | None:
  source = pathlib.Path(path)
  if not source.is_absolute():
    return source.as_posix()
  try:
    return source.resolve().relative_to(repo_root).as_posix()
  except ValueError:
    return None


def ParseTracefile(
    tracefile: pathlib.Path, changed: dict[str, set[int]], repo_root: pathlib.Path
) -> tuple[
    dict[tuple[str, int], int],
    dict[tuple[str, int, str, str], int | None],
    set[str],
]:
  lines: dict[tuple[str, int], int] = {}
  branches: dict[tuple[str, int, str, str], int | None] = {}
  seen_changed_sources: set[str] = set()
  current_file: str | None = None

  with tracefile.open(encoding="utf-8") as stream:
    for raw_line in stream:
      record = raw_line.rstrip("\n")
      if record.startswith("SF:"):
        source = NormalizeSource(record[3:], repo_root)
        if source in changed:
          seen_changed_sources.add(source)
          current_file = source
        else:
          current_file = None
        continue

      if current_file is None:
        continue

      if record.startswith("DA:"):
        fields = record[3:].split(",")
        line_no = int(fields[0])
        if line_no not in changed[current_file]:
          continue
        count = int(fields[1])
        key = (current_file, line_no)
        lines[key] = max(lines.get(key, 0), count)
        continue

      if record.startswith("BRDA:"):
        fields = record[5:].split(",")
        if len(fields) != 4:
          continue
        line_no = int(fields[0])
        if line_no not in changed[current_file]:
          continue
        taken = None if fields[3] == "-" else int(fields[3])
        key = (current_file, line_no, fields[1], fields[2])
        previous = branches.get(key)
        if previous is None or (taken is not None and taken > previous):
          branches[key] = taken

  return lines, branches, seen_changed_sources


def Percentage(covered: int, total: int) -> float:
  return 100.0 if total == 0 else 100.0 * covered / total


def main() -> int:
  args = ParseArgs()
  repo_root = pathlib.Path.cwd().resolve()
  tracefile = pathlib.Path(args.tracefile)
  if not tracefile.is_file():
    print(f"ERROR: coverage tracefile not found: {tracefile}", file=sys.stderr)
    return 2

  changed = ChangedLines(args.base, args.source_prefix)
  lines, branches, seen_changed_sources = ParseTracefile(tracefile, changed, repo_root)

  compiled_source_suffixes = {".c", ".cc", ".cpp", ".cxx"}
  missing_compiled_sources = sorted(
      path
      for path in changed
      if pathlib.PurePosixPath(path).suffix in compiled_source_suffixes and path not in seen_changed_sources
  )
  if missing_compiled_sources:
    print(
        "ERROR: changed compiled production sources are missing from the LCOV trace:",
        file=sys.stderr,
    )
    for path in missing_compiled_sources:
      print(f"  {path}", file=sys.stderr)
    return 2

  line_total = len(lines)
  line_covered = sum(count > 0 for count in lines.values())
  branch_total = len(branches)
  branch_covered = sum(taken is not None and taken > 0 for taken in branches.values())
  line_percent = Percentage(line_covered, line_total)
  branch_percent = Percentage(branch_covered, branch_total)

  print(f"Incremental production coverage against {args.base}:")
  print(f"  lines:    {line_covered}/{line_total} = {line_percent:.2f}%")
  print(f"  branches: {branch_covered}/{branch_total} = {branch_percent:.2f}%")
  if args.branch_threshold is None:
    print("  branch gate: report-only; review uncovered critical logic branches explicitly")

  missed_lines: dict[str, list[int]] = collections.defaultdict(list)
  for (path, line_no), count in sorted(lines.items()):
    if count == 0:
      missed_lines[path].append(line_no)
  if missed_lines:
    print("Uncovered changed production lines:")
    for path, line_numbers in missed_lines.items():
      shown = ", ".join(str(line_no) for line_no in line_numbers[:30])
      suffix = " ..." if len(line_numbers) > 30 else ""
      print(f"  {path}: {shown}{suffix}")

  missed_branches: dict[str, list[int]] = collections.defaultdict(list)
  for (path, line_no, _block, _branch), taken in sorted(branches.items()):
    if taken is None or taken == 0:
      missed_branches[path].append(line_no)
  if missed_branches:
    print("Uncovered changed production branches (source line per branch):")
    for path, line_numbers in missed_branches.items():
      shown = ", ".join(str(line_no) for line_no in line_numbers[:30])
      suffix = " ..." if len(line_numbers) > 30 else ""
      print(f"  {path}: {shown}{suffix}")

  failed = False
  if line_percent < args.line_threshold:
    print(
        f"ERROR: incremental line coverage {line_percent:.2f}% is below {args.line_threshold:.2f}%",
        file=sys.stderr,
    )
    failed = True
  if args.branch_threshold is not None and branch_percent < args.branch_threshold:
    print(
        f"ERROR: incremental branch coverage {branch_percent:.2f}% is below {args.branch_threshold:.2f}%",
        file=sys.stderr,
    )
    failed = True
  return 1 if failed else 0


if __name__ == "__main__":
  sys.exit(main())
