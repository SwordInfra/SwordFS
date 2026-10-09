#!/usr/bin/env python3
"""Contract checks for the conservative GitHub Actions documentation gate."""

import importlib.util
import pathlib
import subprocess
import tempfile
import unittest


SOURCE = pathlib.Path(__file__).resolve().parents[2] / "scripts/ci/docs_only.py"
SPEC = importlib.util.spec_from_file_location("docs_only", SOURCE)
assert SPEC is not None and SPEC.loader is not None
docs_only = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(docs_only)


class DocsOnlyTest(unittest.TestCase):
    def test_document_path_allowlist(self) -> None:
        for path in ("README.md", "docs/design/architecture.md", "docs/guide/index.mdx", "docs/api.rst"):
            self.assertTrue(docs_only.is_documentation_path(path), path)
        for path in (
            "src/file.cpp",
            "tests/test.py",
            "scripts/build.sh",
            ".github/workflows/ci.yml",
            "docs/examples/benchmark.py",
            "docs/config.yaml",
            "README.md.bak",
            "docs/../src/file.md",
            "docs/file.md/../../README.md",
        ):
            self.assertFalse(docs_only.is_documentation_path(path), path)

    def test_empty_or_mixed_diff_requires_full_ci(self) -> None:
        self.assertTrue(docs_only.requires_full_ci([]))
        self.assertFalse(docs_only.requires_full_ci(["README.md", "docs/design/architecture.md"]))
        self.assertTrue(docs_only.requires_full_ci(["docs/architecture.md", "src/file.hpp"]))

    def test_git_merge_base_includes_all_commits_and_rename_sources(self) -> None:
        with tempfile.TemporaryDirectory() as root:
            repo = pathlib.Path(root)

            def git(*args: str) -> str:
                return subprocess.check_output(["git", "-C", root, *args], text=True).strip()

            def commit(name: str, contents: str) -> None:
                target = repo / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(contents, encoding="utf-8")
                git("add", "-A")
                git("-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-qm", "fixture")

            git("init", "-q")
            commit("src/file.cpp", "int main() { return 0; }\n")
            base = git("rev-parse", "HEAD")
            git("switch", "-qc", "docs-only")
            commit("docs/a.md", "hello\n")
            # A PR may contain later docs commits; classification uses the full merge-base diff.
            commit("docs/b.md", "world\n")
            head = git("rev-parse", "HEAD")
            original = pathlib.Path.cwd()
            try:
                import os

                os.chdir(repo)
                self.assertFalse(docs_only.requires_full_ci(docs_only.changed_paths(base, head)))
                (repo / "docs/moved.md").write_text((repo / "src/file.cpp").read_text())
                (repo / "src/file.cpp").unlink()
                git("add", "-A")
                git("-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-qm", "rename")
                head = git("rev-parse", "HEAD")
                self.assertTrue(docs_only.requires_full_ci(docs_only.changed_paths(base, head)))
            finally:
                os.chdir(original)


if __name__ == "__main__":
    unittest.main()
