#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD_DIR="$ROOT_DIR/build"
OUTPUT="$ROOT_DIR/build/static-audit.json"
SUPPRESSIONS="$ROOT_DIR/scripts/static_audit_suppressions.json"

usage() {
  cat <<'EOF'
Usage: scripts/run-static-audit.sh [options]

Options:
  --build-dir DIR      Configured CMake build directory (default: build)
  --output FILE        Semantic-audit JSON output (default: build/static-audit.json)
  --suppressions FILE  Exact semantic suppressions JSON
  -h, --help           Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR=$2
      shift 2
      ;;
    --output)
      OUTPUT=$2
      shift 2
      ;;
    --suppressions)
      SUPPRESSIONS=$2
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

BUILD_DIR=$(cd "$BUILD_DIR" && pwd)
if [[ ! -f "$BUILD_DIR/compile_commands.json" ]]; then
  echo "Missing $BUILD_DIR/compile_commands.json; configure SwordFS first." >&2
  exit 2
fi

PYTHON_BIN=${STATIC_AUDIT_PYTHON:-/usr/bin/python3}
if [[ ! -x "$PYTHON_BIN" ]]; then
  PYTHON_BIN=$(command -v python3 || true)
fi
if [[ -z "$PYTHON_BIN" || ! -x "$PYTHON_BIN" ]]; then
  echo "Python 3 is required for the semantic symbol audit." >&2
  exit 2
fi

symbol_status=0
PYTHON_COMMAND=("$PYTHON_BIN")
if [[ "${STATIC_AUDIT_COVERAGE:-0}" == "1" ]]; then
  if ! "$PYTHON_BIN" -c 'import coverage' >/dev/null 2>&1; then
    echo "coverage must be importable by $PYTHON_BIN when STATIC_AUDIT_COVERAGE=1." >&2
    exit 2
  fi
  PYTHON_COMMAND=(
    "$PYTHON_BIN" -m coverage run --append --branch
    "--include=$ROOT_DIR/scripts/symbol_audit.py"
  )
fi

"${PYTHON_COMMAND[@]}" "$ROOT_DIR/scripts/symbol_audit.py" \
  --repo-root "$ROOT_DIR" \
  --build-dir "$BUILD_DIR" \
  --suppressions "$SUPPRESSIONS" \
  --output "$OUTPUT" || symbol_status=$?

tidy_status=0
if ! command -v run-clang-tidy >/dev/null 2>&1; then
  echo "run-clang-tidy is required for the static audit." >&2
  tidy_status=2
else
  run-clang-tidy \
    -p "$BUILD_DIR" \
    -j "$(nproc)" \
    -checks='-*,clang-analyzer-deadcode.DeadStores,misc-unused-using-decls' \
    -warnings-as-errors='*' \
    -header-filter='.*/src/.*' \
    '.*/src/.*\.(cc|cpp|cxx)$' || tidy_status=$?
fi

if [[ $symbol_status -eq 2 || $tidy_status -eq 2 ]]; then
  exit 2
fi
if [[ $symbol_status -ne 0 || $tidy_status -ne 0 ]]; then
  exit 1
fi
