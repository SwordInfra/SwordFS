#!/usr/bin/env python3
"""Whole-repository semantic liveness audit for SwordFS production symbols."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import multiprocessing
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


class AuditError(RuntimeError):
    """Static-audit configuration or parsing failure."""


@dataclass(frozen=True, order=True)
class Location:
    file: str
    line: int
    column: int

    def as_dict(self) -> dict[str, Any]:
        return {"file": self.file, "line": self.line, "column": self.column}


@dataclass
class Symbol:
    usr: str
    spelling: str
    qualified_name: str
    kind: str
    declarations: set[Location] = field(default_factory=set)
    definitions: set[Location] = field(default_factory=set)
    production_references: set[Location] = field(default_factory=set)
    test_references: set[Location] = field(default_factory=set)
    other_references: set[Location] = field(default_factory=set)


@dataclass(frozen=True)
class CompileUnit:
    directory: Path
    source: Path
    arguments: tuple[str, ...]


def _load_clang() -> Any:
    try:
        from clang import cindex  # type: ignore[import-not-found]
    except ImportError as exc:
        raise AuditError(
            "libclang Python bindings are unavailable; install the distro "
            "python3-clang package and run this tool with /usr/bin/python3"
        ) from exc
    return cindex


def _relative_path(path: Path, repo_root: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(repo_root).as_posix()
    except ValueError:
        return resolved.as_posix()


def _location(cursor: Any, repo_root: Path) -> Location | None:
    location = cursor.location
    if location is None or location.file is None:
        return None
    return Location(
        _relative_path(Path(location.file.name), repo_root),
        int(location.line),
        int(location.column),
    )


def _is_under(path: str, directory: str) -> bool:
    return path == directory or path.startswith(f"{directory}/")


def _is_repository_ast_path(path: str) -> bool:
    if Path(path).is_absolute():
        return False
    first_component = path.split("/", 1)[0]
    if first_component in {".deps", "build", "third_party"}:
        return False
    return not first_component.startswith("build-")


def _qualified_name(cursor: Any, cindex: Any) -> str:
    name = cursor.displayname or cursor.spelling
    parents: list[str] = []
    parent = cursor.semantic_parent
    while parent is not None and parent.kind != cindex.CursorKind.TRANSLATION_UNIT:
        if parent.spelling:
            parents.append(parent.spelling)
        parent = parent.semantic_parent
    parents.reverse()
    parents.append(name)
    return "::".join(parents)


def _eligible_declaration(cursor: Any, cindex: Any, repo_root: Path) -> bool:
    location = _location(cursor, repo_root)
    if location is None or not _is_under(location.file, "src"):
        return False

    parent = cursor.semantic_parent
    if parent is None:
        return False

    namespace_parents = {
        cindex.CursorKind.TRANSLATION_UNIT,
        cindex.CursorKind.NAMESPACE,
    }
    record_parents = {
        cindex.CursorKind.CLASS_DECL,
        cindex.CursorKind.STRUCT_DECL,
        cindex.CursorKind.UNION_DECL,
    }

    if cursor.kind == cindex.CursorKind.FUNCTION_DECL:
        if cursor.spelling == "main" or cursor.spelling.startswith("operator"):
            return False
        return parent.kind in namespace_parents

    if cursor.kind == cindex.CursorKind.CXX_METHOD:
        if parent.kind not in record_parents:
            return False
        if cursor.spelling.startswith("operator"):
            return False
        try:
            if cursor.is_virtual_method():
                return False
        except Exception:
            return False
        return True

    if cursor.kind in {
        cindex.CursorKind.TYPE_ALIAS_DECL,
        cindex.CursorKind.TYPEDEF_DECL,
    }:
        return parent.kind in namespace_parents | record_parents

    return False


def _is_alias(symbol: Symbol) -> bool:
    return symbol.kind in {"TYPE_ALIAS_DECL", "TYPEDEF_DECL"}


def _requires_definition(symbol: Symbol) -> bool:
    return symbol.kind in {"FUNCTION_DECL", "CXX_METHOD"}


def _is_indirect_protocol_method(symbol: Symbol) -> bool:
    """Return true for methods commonly invoked through library lock wrappers.

    std::lock_guard/std::unique_lock/std::shared_lock instantiate calls to these
    protocol methods in standard-library headers. The repository-only AST walk
    intentionally prunes those headers for performance, so absence of a direct
    repository reference is not strong enough evidence to classify them as
    dead.
    """
    return symbol.kind == "CXX_METHOD" and symbol.spelling in {
        "lock",
        "try_lock",
        "unlock",
        "lock_shared",
        "try_lock_shared",
        "unlock_shared",
    }


def _reference_kinds(cindex: Any) -> set[Any]:
    kinds = {cindex.CursorKind.DECL_REF_EXPR, cindex.CursorKind.MEMBER_REF_EXPR}
    for name in ("MEMBER_REF", "VARIABLE_REF"):
        kind = getattr(cindex.CursorKind, name, None)
        if kind is not None:
            kinds.add(kind)
    return kinds


def _walk(cursor: Any, repo_root: Path) -> Iterable[Any]:
    yield cursor
    for child in cursor.get_children():
        location = _location(child, repo_root)
        if location is not None and not _is_repository_ast_path(location.file):
            continue
        yield from _walk(child, repo_root)


def _compile_arguments(unit: CompileUnit) -> list[str]:
    argv = list(unit.arguments)
    while argv and Path(argv[0]).name in {"ccache", "sccache"}:
        argv.pop(0)
    if not argv:
        raise AuditError(f"empty compile command for {unit.source}")
    argv.pop(0)

    result: list[str] = []
    skip_next = False
    options_with_value_to_drop = {"-o", "-MF", "-MT", "-MQ"}
    flags_to_drop = {"-c", "-MD", "-MMD"}
    source_resolved = unit.source.resolve()

    for argument in argv:
        if skip_next:
            skip_next = False
            continue
        if argument in options_with_value_to_drop:
            skip_next = True
            continue
        if argument in flags_to_drop:
            continue

        if not argument.startswith("-"):
            argument_path = Path(argument)
            if not argument_path.is_absolute():
                argument_path = unit.directory / argument_path
            try:
                if argument_path.resolve() == source_resolved:
                    continue
            except OSError:
                pass
        result.append(argument)

    return result


def _load_compile_commands(build_dir: Path, repo_root: Path, cindex: Any) -> list[CompileUnit]:
    compile_db = build_dir / "compile_commands.json"
    if not compile_db.is_file():
        raise AuditError(
            f"missing {compile_db}; configure SwordFS first so CMake generates the current compilation database"
        )
    try:
        database = cindex.CompilationDatabase.fromDirectory(build_dir.as_posix())
        commands = database.getAllCompileCommands()
    except Exception as exc:
        raise AuditError(f"libclang cannot read compilation database {compile_db}: {exc}") from exc

    selected: dict[str, CompileUnit] = {}
    for command in commands:
        directory = Path(command.directory).resolve()
        source = Path(command.filename)
        if not source.is_absolute():
            source = directory / source
        relative = _relative_path(source, repo_root)
        if not (_is_under(relative, "src") or _is_under(relative, "tests")):
            continue
        if source.suffix not in {".cc", ".cpp", ".cxx"}:
            continue
        selected.setdefault(
            source.resolve().as_posix(),
            CompileUnit(
                directory=directory,
                source=source.resolve(),
                arguments=tuple(command.arguments),
            ),
        )

    if not selected:
        raise AuditError("compilation database contains no src/ or tests/ translation units")
    return [selected[key] for key in sorted(selected)]


def _record_reference(symbols: dict[str, Symbol], usr: str, location: Location) -> None:
    symbol = symbols.get(usr)
    if symbol is None:
        return
    if _is_under(location.file, "src"):
        symbol.production_references.add(location)
    elif _is_under(location.file, "tests"):
        symbol.test_references.add(location)
    else:
        symbol.other_references.add(location)


def _collect_translation_unit(
    tu: Any,
    symbols: dict[str, Symbol],
    pending_references: dict[str, set[Location]],
    cindex: Any,
    repo_root: Path,
) -> None:
    reference_expressions = _reference_kinds(cindex)
    for cursor in _walk(tu.cursor, repo_root):
        if cursor.kind.is_declaration() and _eligible_declaration(cursor, cindex, repo_root):
            usr = cursor.get_usr()
            location = _location(cursor, repo_root)
            if usr and location is not None:
                symbol = symbols.setdefault(
                    usr,
                    Symbol(
                        usr=usr,
                        spelling=cursor.spelling,
                        qualified_name=_qualified_name(cursor, cindex),
                        kind=cursor.kind.name,
                    ),
                )
                symbol.declarations.add(location)
                try:
                    if cursor.is_definition():
                        symbol.definitions.add(location)
                except Exception:
                    pass

        is_reference = cursor.kind.is_reference() or cursor.kind in reference_expressions
        if not is_reference:
            continue
        try:
            referenced = cursor.referenced
        except Exception:
            referenced = None
        if referenced is None:
            continue
        usr = referenced.get_usr()
        location = _location(cursor, repo_root)
        if not usr or location is None:
            continue
        pending_references.setdefault(usr, set()).add(location)


def _parse_compile_unit(
    unit: CompileUnit,
    repo_root: Path,
) -> tuple[dict[str, dict[str, Any]], dict[str, list[tuple[str, int, int]]]]:
    cindex = _load_clang()
    index = cindex.Index.create()
    symbols: dict[str, Symbol] = {}
    pending_references: dict[str, set[Location]] = {}

    original_cwd = Path.cwd()
    try:
        arguments = _compile_arguments(unit)
        os.chdir(unit.directory)
        try:
            tu = index.parse(unit.source.as_posix(), args=arguments)
        except Exception as exc:
            raise AuditError(
                f"libclang failed to parse {_relative_path(unit.source, repo_root)}: {exc}"
            ) from exc

        errors = [
            diagnostic
            for diagnostic in tu.diagnostics
            if diagnostic.severity >= cindex.Diagnostic.Error
        ]
        if errors:
            details = "\n".join(str(diagnostic) for diagnostic in errors[:20])
            raise AuditError(
                f"libclang reported parse errors for {_relative_path(unit.source, repo_root)}:\n{details}"
            )
        _collect_translation_unit(tu, symbols, pending_references, cindex, repo_root)
    finally:
        os.chdir(original_cwd)

    serialized_symbols = {
        usr: {
            "usr": symbol.usr,
            "spelling": symbol.spelling,
            "qualified_name": symbol.qualified_name,
            "kind": symbol.kind,
            "declarations": [
                (location.file, location.line, location.column)
                for location in sorted(symbol.declarations)
            ],
            "definitions": [
                (location.file, location.line, location.column)
                for location in sorted(symbol.definitions)
            ],
        }
        for usr, symbol in symbols.items()
    }
    serialized_references = {
        usr: [
            (location.file, location.line, location.column)
            for location in sorted(locations)
        ]
        for usr, locations in pending_references.items()
    }
    return serialized_symbols, serialized_references


def _merge_parse_result(
    symbols: dict[str, Symbol],
    pending_references: dict[str, set[Location]],
    unit_symbols: dict[str, dict[str, Any]],
    unit_references: dict[str, list[tuple[str, int, int]]],
) -> None:
    for usr, incoming in unit_symbols.items():
        symbol = symbols.setdefault(
            usr,
            Symbol(
                usr=incoming["usr"],
                spelling=incoming["spelling"],
                qualified_name=incoming["qualified_name"],
                kind=incoming["kind"],
            ),
        )
        symbol.declarations.update(Location(*location) for location in incoming["declarations"])
        symbol.definitions.update(Location(*location) for location in incoming["definitions"])

    for usr, locations in unit_references.items():
        pending_references.setdefault(usr, set()).update(
            Location(*location) for location in locations
        )


def _parse_repository(
    repo_root: Path,
    build_dir: Path,
    jobs: int,
) -> tuple[dict[str, Symbol], int]:
    cindex = _load_clang()
    units = _load_compile_commands(build_dir, repo_root, cindex)
    symbols: dict[str, Symbol] = {}
    pending_references: dict[str, set[Location]] = {}

    if jobs <= 1 or len(units) == 1:
        results = (_parse_compile_unit(unit, repo_root) for unit in units)
        for unit_symbols, unit_references in results:
            _merge_parse_result(symbols, pending_references, unit_symbols, unit_references)
    else:
        worker_count = min(jobs, len(units))
        context = multiprocessing.get_context("spawn")
        with concurrent.futures.ProcessPoolExecutor(
            max_workers=worker_count,
            mp_context=context,
        ) as executor:
            futures = [executor.submit(_parse_compile_unit, unit, repo_root) for unit in units]
            for future in futures:
                unit_symbols, unit_references = future.result()
                _merge_parse_result(symbols, pending_references, unit_symbols, unit_references)

    for usr, locations in pending_references.items():
        for location in locations:
            _record_reference(symbols, usr, location)
    return symbols, len(units)


def _primary_location(locations: set[Location]) -> Location:
    return sorted(locations)[0]


def _finding_for_symbol(symbol: Symbol) -> dict[str, Any] | None:
    if _requires_definition(symbol) and not symbol.definitions:
        return None

    production = sorted(symbol.production_references)
    tests = sorted(symbol.test_references)
    other = sorted(symbol.other_references)

    if production:
        return None

    if _is_indirect_protocol_method(symbol):
        category = "indirect-protocol-method"
        confidence = "review"
    elif not tests and not other:
        if _is_alias(symbol):
            category = "unused-production-alias"
        else:
            category = "zero-reference-production-symbol"
        confidence = "high"
    elif tests and not other:
        category = "test-only-production-symbol"
        confidence = "high"
    else:
        category = "non-production-only-production-symbol"
        confidence = "review"

    declaration = _primary_location(symbol.declarations)
    definition = _primary_location(symbol.definitions) if symbol.definitions else None
    return {
        "category": category,
        "confidence": confidence,
        "usr": symbol.usr,
        "spelling": symbol.spelling,
        "symbol": symbol.qualified_name,
        "kind": symbol.kind,
        "declaration": declaration.as_dict(),
        "definition": definition.as_dict() if definition else None,
        "production_reference_count": len(production),
        "production_references": [location.as_dict() for location in production],
        "test_reference_count": len(tests),
        "test_references": [location.as_dict() for location in tests],
        "other_reference_count": len(other),
        "other_references": [location.as_dict() for location in other],
        "suppressed": False,
    }


def _load_suppressions(path: Path) -> list[dict[str, str]]:
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise AuditError(f"suppression file does not exist: {path}") from exc
    except json.JSONDecodeError as exc:
        raise AuditError(f"invalid suppression JSON in {path}: {exc}") from exc
    if not isinstance(raw, list):
        raise AuditError("static-audit suppressions must be a JSON array")

    suppressions: list[dict[str, str]] = []
    required = {"category", "usr", "symbol", "reason"}
    for index, entry in enumerate(raw):
        if not isinstance(entry, dict) or set(entry) != required:
            raise AuditError(
                f"suppression #{index + 1} must contain exactly: {', '.join(sorted(required))}"
            )
        if any(not isinstance(entry[key], str) or not entry[key].strip() for key in required):
            raise AuditError(f"suppression #{index + 1} contains an empty or non-string field")
        suppressions.append(entry)
    return suppressions


def _apply_suppressions(
    findings: list[dict[str, Any]], suppressions: list[dict[str, str]]
) -> None:
    matched: set[int] = set()
    for finding in findings:
        for index, suppression in enumerate(suppressions):
            if (
                finding["category"] == suppression["category"]
                and finding["usr"] == suppression["usr"]
                and finding["symbol"] == suppression["symbol"]
            ):
                finding["suppressed"] = True
                finding["suppression_reason"] = suppression["reason"]
                matched.add(index)
                break

    stale = [suppressions[index] for index in range(len(suppressions)) if index not in matched]
    if stale:
        details = "\n".join(
            f"  - {entry['category']} {entry['symbol']} ({entry['usr']})" for entry in stale
        )
        raise AuditError(f"stale static-audit suppressions must be removed:\n{details}")


def analyze(
    repo_root: Path,
    build_dir: Path,
    suppressions_path: Path,
    jobs: int = 1,
) -> dict[str, Any]:
    repo_root = repo_root.resolve()
    build_dir = build_dir.resolve()
    if jobs < 1:
        raise AuditError("static-audit jobs must be at least 1")
    symbols, translation_unit_count = _parse_repository(repo_root, build_dir, jobs)
    findings = [
        finding
        for symbol in symbols.values()
        if (finding := _finding_for_symbol(symbol)) is not None
    ]
    findings.sort(key=lambda finding: (finding["category"], finding["symbol"], finding["usr"]))
    suppressions = _load_suppressions(suppressions_path)
    _apply_suppressions(findings, suppressions)

    blocking = [
        finding
        for finding in findings
        if finding["confidence"] == "high" and not finding["suppressed"]
    ]
    review = [
        finding
        for finding in findings
        if finding["confidence"] == "review" and not finding["suppressed"]
    ]
    suppressed = [finding for finding in findings if finding["suppressed"]]

    return {
        "schema_version": 1,
        "translation_unit_count": translation_unit_count,
        "summary": {
            "blocking_high_confidence": len(blocking),
            "review_candidates": len(review),
            "suppressed": len(suppressed),
            "total_findings": len(findings),
        },
        "findings": findings,
    }


def _github_escape(value: str, property_value: bool = False) -> str:
    escaped = value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    if property_value:
        escaped = escaped.replace(":", "%3A").replace(",", "%2C")
    return escaped


def _emit_github_annotation(finding: dict[str, Any]) -> None:
    if os.environ.get("GITHUB_ACTIONS") != "true" or finding["suppressed"]:
        return
    location = finding["definition"] or finding["declaration"]
    level = "error" if finding["confidence"] == "high" else "warning"
    title = "SwordFS static audit"
    message = (
        f"{finding['category']}: {finding['symbol']} "
        f"(production refs={finding['production_reference_count']}, "
        f"test refs={finding['test_reference_count']})"
    )
    print(
        f"::{level} file={_github_escape(location['file'], True)},"
        f"line={location['line']},col={location['column']},"
        f"title={_github_escape(title, True)}::{_github_escape(message)}"
    )


def _format_finding(finding: dict[str, Any]) -> str:
    location = finding["definition"] or finding["declaration"]
    status = "SUPPRESSED" if finding["suppressed"] else finding["confidence"].upper()
    return (
        f"[{status}] {finding['category']}: {finding['symbol']} "
        f"at {location['file']}:{location['line']} "
        f"(prod={finding['production_reference_count']}, "
        f"test={finding['test_reference_count']}, other={finding['other_reference_count']})"
    )


def _write_github_summary(report: dict[str, Any], output: Path) -> None:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    summary = report["summary"]
    lines = [
        "## SwordFS static symbol audit",
        "",
        f"- Translation units: {report['translation_unit_count']}",
        f"- Blocking high-confidence findings: {summary['blocking_high_confidence']}",
        f"- Review candidates: {summary['review_candidates']}",
        f"- Suppressed findings: {summary['suppressed']}",
        f"- JSON report: `{output.as_posix()}`",
    ]
    visible = [finding for finding in report["findings"] if not finding["suppressed"]]
    if visible:
        lines.extend(["", "### Findings", ""])
        for finding in visible[:50]:
            lines.append(f"- `{finding['category']}` — `{finding['symbol']}`")
        if len(visible) > 50:
            lines.append(f"- … {len(visible) - 50} more; inspect the JSON artifact")
    with Path(summary_path).open("a", encoding="utf-8") as summary_file:
        summary_file.write("\n".join(lines) + "\n")


def _parse_args() -> argparse.Namespace:
    repo_root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=repo_root)
    parser.add_argument("--build-dir", type=Path, default=repo_root / "build")
    parser.add_argument(
        "--suppressions",
        type=Path,
        default=repo_root / "scripts" / "static-analysis" / "suppressions.json",
    )
    parser.add_argument("--output", type=Path, default=repo_root / "build" / "static-audit.json")
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(4, os.cpu_count() or 1),
        help="translation units parsed in parallel (default: up to 4)",
    )
    parser.add_argument(
        "--no-fail",
        action="store_true",
        help="report blocking findings without returning a failing status (fixture/debug use)",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    try:
        report = analyze(args.repo_root, args.build_dir, args.suppressions, jobs=args.jobs)
    except AuditError as exc:
        print(f"static symbol audit error: {exc}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    if not report["findings"]:
        print("Static symbol audit: no dead/test-only production candidates found.")
    else:
        for finding in report["findings"]:
            print(_format_finding(finding))
            _emit_github_annotation(finding)

    summary = report["summary"]
    print(
        "Static symbol audit summary: "
        f"blocking={summary['blocking_high_confidence']} "
        f"review={summary['review_candidates']} "
        f"suppressed={summary['suppressed']}"
    )
    _write_github_summary(report, args.output)

    if summary["blocking_high_confidence"] and not args.no_fail:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
