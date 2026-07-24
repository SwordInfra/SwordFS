#!/bin/bash
# SwordFS end-to-end smoke test.
#
# Tests: format → mount → write → read → verify → unmount
# Uses the memory backend (no S3 required) for fast CI execution.
#
# Usage:
#   ./tests/integration/smoke_test.sh [swordfs_binary] [mountpoint] [testdir]

set -euo pipefail

SWORDFS="${1:-./build/swordfs}"
MNT="${2:-/tmp/swordfs_smoke_test_$$}"
TESTDIR="${3:-/tmp/swordfs_test_$$}"
FORMAT_DIR="${TESTDIR}/volume"

PASS=0
FAIL=0

cleanup() {
  fusermount3 -u "$MNT" 2>/dev/null || true
  rm -rf "$TESTDIR" 2>/dev/null || true
}
trap cleanup EXIT

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "==> Building SwordFS..."
cmake --preset default -DENABLE_S3=OFF 2>&1 | tail -1
cmake --build build --target swordfs 2>&1 | tail -1
echo ""

echo "==> Test 1: format volume"
mkdir -p "$FORMAT_DIR"
if "$SWORDFS" format "$FORMAT_DIR" 2>&1; then
  pass "format succeeded"
  if [ -f "$FORMAT_DIR/volume.json" ]; then
    pass "volume.json created"
  else
    fail "volume.json not found"
  fi
else
  fail "format failed"
fi

echo ""
echo "==> Test 2: mount volume"
mkdir -p "$MNT"
if "$SWORDFS" mount --volume "$FORMAT_DIR" "$MNT" -o allow_other 2>&1 &
then
  sleep 1
  if mountpoint -q "$MNT"; then
    pass "mount succeeded"
  else
    fail "mountpoint check failed"
  fi
else
  fail "mount command failed"
fi

echo ""
echo "==> Test 3: create file and write data"
TEST_FILE="$MNT/hello.txt"
if echo "Hello SwordFS!" > "$TEST_FILE" 2>&1; then
  pass "write succeeded"
  if [ -f "$TEST_FILE" ]; then
    pass "file exists after write"
  else
    fail "file not found after write"
  fi
else
  fail "write failed"
fi

echo ""
echo "==> Test 4: read data back"
if [ -f "$TEST_FILE" ]; then
  CONTENT=$(cat "$TEST_FILE")
  if [ "$CONTENT" = "Hello SwordFS!" ]; then
    pass "read data matches"
  else
    fail "read data mismatch: got '$CONTENT'"
  fi
else
  fail "file disappeared"
fi

echo ""
echo "==> Test 5: create directory and nested file"
mkdir -p "$MNT/subdir"
echo "nested" > "$MNT/subdir/nested.txt"
if [ -f "$MNT/subdir/nested.txt" ]; then
  pass "nested file created"
  NESTED=$(cat "$MNT/subdir/nested.txt")
  if [ "$NESTED" = "nested" ]; then
    pass "nested content matches"
  else
    fail "nested content mismatch"
  fi
else
  fail "nested file not found"
fi

echo ""
echo "==> Test 6: unmount"
fusermount3 -u "$MNT" 2>&1 || true
sleep 1
if ! mountpoint -q "$MNT" 2>/dev/null; then
  pass "unmount succeeded"
else
  fail "unmount failed"
fi

echo ""
echo "========================================="
echo "Results: $PASS passed, $FAIL failed"
echo "========================================="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
