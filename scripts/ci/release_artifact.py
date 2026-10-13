#!/usr/bin/env python3
"""Create and verify the portable Release runtime distributed within a CI run."""

import argparse
import hashlib
import json
import pathlib
import platform
import re
import shutil
import subprocess
import tarfile
import tempfile

# The loader and glibc must come from the same runner image.
GLIBC = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2",
         "librt.so.1", "libresolv.so.2", "libutil.so.1", "ld-linux-x86-64.so.2"}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def check_runtime(binary):
    output = subprocess.check_output(["ldd", "-r", str(binary)], text=True, stderr=subprocess.STDOUT)
    if "not found" in output or "undefined symbol" in output:
        raise ValueError("unresolved runtime library:\n" + output)
    subprocess.run([str(binary), "--help"], stdout=subprocess.DEVNULL, check=True)


def create(args):
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        libdir = root / "build/lib"
        libdir.mkdir(parents=True)
        binary = root / "build/swordfs"
        shutil.copy2(args.binary, binary)
        binary.chmod(0o755)
        linkage = subprocess.check_output(["ldd", str(args.binary)], text=True)
        if "not found" in linkage:
            raise ValueError("missing build-time shared library:\n" + linkage)
        for line in linkage.splitlines():
            match = re.search(r"^\s*(\S+)\s+=>\s+(/\S+)", line)
            if not match or match.group(1) in GLIBC:
                continue
            name, source = match.group(1), pathlib.Path(match.group(2))
            if not re.fullmatch(r"[\w.+-]+", name) or not source.is_file():
                raise ValueError(f"unsafe shared library: {line}")
            target = libdir / name
            if target.exists():
                if sha256(source) != sha256(target):
                    raise ValueError(f"library name collision: {name}")
                continue
            shutil.copy2(source, target)
            target.chmod(0o755)
            subprocess.run(["patchelf", "--set-rpath", "$ORIGIN", str(target)], check=True)
        subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/lib", str(binary)], check=True)
        check_runtime(binary)
        manifest = {
            "schema": 1, "sha": args.sha, "build_type": "Release", "coverage": False,
            "run_id": args.run_id, "run_attempt": args.run_attempt,
            "runner_image": args.runner_image, "arch": platform.machine(),
            "files": {
                file.relative_to(root).as_posix(): sha256(file)
                for file in sorted((root / "build").rglob("*")) if file.is_file()
            },
        }
        (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(args.output, "w:gz") as archive:
            for file in [root / "manifest.json", *sorted((root / "build").rglob("*"))]:
                archive.add(file, arcname=file.relative_to(root).as_posix(), recursive=False)
    print(f"archive_sha256={sha256(args.output)} bytes={args.output.stat().st_size}")


def verify(args):
    if sha256(args.archive) != args.artifact_sha:
        raise ValueError("archive digest differs from trusted producer job output")
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        with tarfile.open(args.archive, "r:gz") as archive:
            for member in archive.getmembers():
                path = pathlib.PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts or not (member.isfile() or member.isdir()):
                    raise ValueError(f"unsafe archive member: {member.name}")
                if path.parts[0] not in ("build", "manifest.json"):
                    raise ValueError(f"unexpected archive path: {member.name}")
            archive.extractall(root, filter="data")
        manifest = json.loads((root / "manifest.json").read_text())
        expected = {
            "schema": 1, "sha": args.sha, "build_type": "Release", "coverage": False,
            "run_id": args.run_id, "run_attempt": args.run_attempt,
            "runner_image": args.runner_image, "arch": platform.machine(),
        }
        for key, value in expected.items():
            if manifest.get(key) != value:
                raise ValueError(f"artifact {key}: expected {value!r}, got {manifest.get(key)!r}")
        files = manifest.get("files")
        actual = {file.relative_to(root).as_posix() for file in root.rglob("*") if file.is_file()}
        if not isinstance(files, dict) or actual != (set(files) | {"manifest.json"}):
            raise ValueError("artifact file inventory mismatch")
        for relative, value in files.items():
            if sha256(root / relative) != value:
                raise ValueError(f"artifact content digest mismatch: {relative}")
        binary = root / "build/swordfs"
        if not binary.is_file() or not binary.stat().st_mode & 0o111:
            raise ValueError("artifact swordfs executable permissions missing")
        check_runtime(binary)
        shutil.copytree(root / "build", args.destination / "build", dirs_exist_ok=True)
    print(f"verified same-run Release swordfs for {args.sha}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("pack")
    pack.add_argument("--binary", required=True, type=pathlib.Path)
    pack.add_argument("--output", required=True, type=pathlib.Path)
    unpack = commands.add_parser("verify")
    unpack.add_argument("--archive", required=True, type=pathlib.Path)
    unpack.add_argument("--artifact-sha", required=True)
    unpack.add_argument("--destination", required=True, type=pathlib.Path)
    for command in (pack, unpack):
        for name in ("sha", "run-id", "run-attempt", "runner-image"):
            command.add_argument("--" + name, required=True)
    args = parser.parse_args()
    (create if args.command == "pack" else verify)(args)


if __name__ == "__main__":
    main()
