#!/usr/bin/env python3
"""Attest and install same-run fstests/liburing tools at their fixed ELF RPATH."""

import argparse
import hashlib
import json
import pathlib
import platform
import shutil
import tarfile
import tempfile

PREFIX = pathlib.Path("/opt/swordfs-fstests-tools")
ARCHIVE_PREFIX = PREFIX.name
REQUIRED = ("xfstests/check", "fstests-local-patches.diff",
            "liburing-install/lib/pkgconfig/liburing.pc")


def sha256(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def assert_contents(root):
    for relative in REQUIRED:
        path = root / relative
        if not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"missing or empty fstests toolchain file: {relative}")
    if not (root / "xfstests/check").stat().st_mode & 0o111:
        raise ValueError("xfstests/check is not executable")
    if not list((root / "liburing-install/lib").glob("liburing.so*")):
        raise ValueError("missing liburing ELF shared library")


def metadata(args):
    return {
        "schema": 1,
        "source_sha": args.sha,
        "run_id": args.run_id,
        "run_attempt": args.run_attempt,
        "runner_image": "ubuntu-24.04",
        "arch": platform.machine(),
        "glibc": platform.libc_ver()[1],
        "root": str(PREFIX),
        "fstests_commit": args.fstests_commit,
        "liburing_commit": args.liburing_commit,
        "patch_sha": args.patch_sha,
    }


def pack(args):
    assert_contents(PREFIX)
    if (PREFIX / "manifest.json").exists():
        raise ValueError("unexpected existing fstests manifest")
    (PREFIX / "manifest.json").write_text(json.dumps(metadata(args), sort_keys=True, indent=2) + "\n")
    args.archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.archive, "w:gz") as archive:
        archive.add(PREFIX, arcname=ARCHIVE_PREFIX)
    print(f"archive_sha256={sha256(args.archive)} bytes={args.archive.stat().st_size}")


def verify(args):
    if sha256(args.archive) != args.artifact_sha:
        raise ValueError("fstests toolchain archive hash mismatch with producer")
    if PREFIX.exists():
        raise ValueError("refusing to overwrite existing fstests toolchain")

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(args.archive, "r:gz") as archive:
            for member in archive.getmembers():
                path = pathlib.PurePosixPath(member.name)
                if (path.is_absolute() or ".." in path.parts
                        or path.parts[0] != ARCHIVE_PREFIX):
                    raise ValueError(f"unexpected toolchain archive path: {member.name}")
            archive.extractall(tmp, filter="data")

        root = pathlib.Path(tmp) / ARCHIVE_PREFIX
        actual = json.loads((root / "manifest.json").read_text())
        for key, expected in metadata(args).items():
            if actual.get(key) != expected:
                raise ValueError(f"fstests toolchain {key} mismatch: {actual.get(key)!r} != {expected!r}")
        assert_contents(root)
        shutil.copytree(root, PREFIX, symlinks=True)
    print(f"verified pinned fstests/liburing toolchain at {PREFIX}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("pack", "verify"):
        command = sub.add_parser(name)
        command.add_argument("--archive", required=True, type=pathlib.Path)
        command.add_argument("--sha", required=True)
        command.add_argument("--run-id", required=True)
        command.add_argument("--run-attempt", required=True)
        command.add_argument("--fstests-commit", required=True)
        command.add_argument("--liburing-commit", required=True)
        command.add_argument("--patch-sha", required=True)
        if name == "verify":
            command.add_argument("--artifact-sha", required=True)
    args = parser.parse_args()
    (pack if args.command == "pack" else verify)(args)


if __name__ == "__main__":
    main()
