#!/usr/bin/env python3
"""Conservatively classify a PR's entire diff for the GitHub Actions CI gate.

Only allowlisted documentation paths may skip expensive CI.  The diff must be
computed across the PR merge base and head, not just the latest commit.
Errors or an empty diff must never be treated as documentation-only.
"""

import argparse
import pathlib
import subprocess


ROOT_DOCUMENTS = {"README.md", "CONTRIBUTING.md", "CHANGELOG.md", "CODE_OF_CONDUCT.md"}
DOCUMENT_SUFFIXES = {".md", ".mdx", ".rst"}


def is_documentation_path(name: str) -> bool:
    path = pathlib.PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts:
        return False
    return name in ROOT_DOCUMENTS or (name.startswith("docs/") and path.suffix in DOCUMENT_SUFFIXES)


def requires_full_ci(paths: list[str]) -> bool:
    return not paths or any(not is_documentation_path(path) for path in paths)


def changed_paths(base: str, head: str) -> list[str]:
    # --no-renames treats moving code to docs as code deletion + docs addition;
    # -z keeps filenames with spaces/newlines unambiguous.
    result = subprocess.run(
        ["git", "diff", "--no-renames", "--name-only", "-z", f"{base}...{head}"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=True,
    )
    return [value.decode("utf-8", errors="surrogateescape") for value in result.stdout.split(b"\0") if value]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--github-output", type=pathlib.Path)
    parser.add_argument("--github-summary", type=pathlib.Path)
    args = parser.parse_args()

    paths = changed_paths(args.base, args.head)
    full = requires_full_ci(paths)
    value = "true" if full else "false"
    summary = (
        f"### CI change scope: {'FULL CI' if full else 'documentation-only'}\n\n"
        f"Compared PR merge base `{args.base}` to head `{args.head}`.\n"
        f"Changed paths: {len(paths)}.\n"
        f"Expensive build, audit and conformance jobs: {'required' if full else 'skipped'}.\n\n"
        "All `push` to `main` and manually dispatched CI runs are always full.\n"
    )
    print(summary)
    for path in paths:
        print(repr(path))
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as output:
            output.write(f"run_full_ci={value}\n")
    if args.github_summary:
        with args.github_summary.open("a", encoding="utf-8") as output:
            output.write(summary)


if __name__ == "__main__":
    main()
