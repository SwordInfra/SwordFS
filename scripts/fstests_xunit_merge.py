#!/usr/bin/env python3
"""Merge one-test fstests XUnit reports into one classifier input."""

from __future__ import annotations

import argparse
import copy
import pathlib
import re
import sys
import xml.etree.ElementTree as ET


TEST_RE = re.compile(r"^[a-z0-9_-]+/[0-9]+$")


class MergeError(ValueError):
    pass


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def load_part(path: pathlib.Path) -> tuple[str, ET.Element]:
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        raise MergeError(f"cannot parse {path}: {exc}") from exc

    cases = [element for element in root.iter() if _local_name(element.tag) == "testcase"]
    if len(cases) != 1:
        raise MergeError(f"{path}: expected exactly one testcase, found {len(cases)}")
    test = cases[0].get("name", "")
    if not TEST_RE.fullmatch(test):
        raise MergeError(f"{path}: invalid testcase id: {test!r}")

    if _local_name(root.tag) == "testsuite":
        suite = root
    else:
        suites = [element for element in root if _local_name(element.tag) == "testsuite"]
        if len(suites) != 1:
            raise MergeError(f"{path}: expected exactly one testsuite, found {len(suites)}")
        suite = suites[0]
    return test, copy.deepcopy(suite)


def merge(parts_dir: pathlib.Path, output: pathlib.Path) -> None:
    if not parts_dir.is_dir():
        raise MergeError(f"XUnit parts directory does not exist: {parts_dir}")

    suites: dict[str, ET.Element] = {}
    for path in sorted(parts_dir.glob("*.xml")):
        test, suite = load_part(path)
        if test in suites:
            raise MergeError(f"duplicate testcase in XUnit parts: {test}")
        suites[test] = suite

    root = ET.Element("testsuites")
    for test in sorted(suites):
        root.append(suites[test])
    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    output.parent.mkdir(parents=True, exist_ok=True)
    tree.write(output, encoding="utf-8", xml_declaration=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--parts-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    try:
        merge(args.parts_dir, args.output)
    except MergeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
