#!/usr/bin/env python3
"""Fail CI when backend-specific Metadata types escape into common consumers.

Only production source code is scanned. Backend-specific Redis code under
src/metadata/redis and Redis-oriented tests are intentionally out of scope.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re


CONSUMERS = ("vfs", "fuse", "volume", "chunk")
SUFFIXES = {".cpp", ".hpp", ".cc", ".h"}
# A qualified Redis symbol, or a Redis metadata include, is an implementation
# dependency. Bare "redis" is not: e.g. ChunkType::kRedisCache is a mechanism
# name rather than access to a Redis metadata backend.
FORBIDDEN = (
    re.compile(r'#\s*include\s*[<"][^">]*(?:metadata/redis/|sw/redis\+\+/|hiredis/)'),
    re.compile(r'\b(?:swordfs::)?metadata::(?:redis::|Redis(?:Meta|Key|Kv|Codec|Backend|COWChunk))'),
    re.compile(r'\busing\s+(?:namespace\s+)?(?:swordfs::)?metadata::redis\b'),
    re.compile(r'\bsw::redis::'),
    re.compile(r'\b(?:RedisMeta(?:Impl|Ops|Txn|Client|Config)|RedisBackendContext|RedisKvTxn|RedisCOWChunkMetadata)\b'),
)


def check(root: Path) -> list[str]:
    violations = []
    for consumer in CONSUMERS:
        for path in sorted((root / "src" / consumer).rglob("*")):
            if not path.is_file() or path.suffix not in SUFFIXES:
                continue
            relative = path.relative_to(root)
            for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                # Comments cannot create a source dependency. This is a focused
                # guard, not a C++ parser; production code checks still apply.
                code = line.split("//", 1)[0]
                if any(pattern.search(code) for pattern in FORBIDDEN):
                    violations.append(f"{relative}:{number}: {line.strip()}")
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    violations = check(args.root)
    if violations:
        print("Concrete Redis Metadata dependency escaped into a common consumer:")
        for violation in violations:
            print(f"  {violation}")
        return 1
    print("Metadata backend dependency boundaries: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
