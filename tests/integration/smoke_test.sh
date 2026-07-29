#!/bin/bash
# SwordFS integration smoke test — format and verify volume creation.
# FUSE mount test is skipped in CI (needs /dev/fuse + kernel module).

set -euo pipefail

SWORDFS="${1:-./build/swordfs}"
TESTDIR="/tmp/swordfs_smoke_test_$$"
FORMAT_DIR="${TESTDIR}/volume"

PASS=0
FAIL=0

cleanup() {
  sudo rm -rf "$TESTDIR" 2>/dev/null || true
}
trap cleanup EXIT

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "==> Building SwordFS..."
cmake --preset default 2>/dev/null
cmake --build build --target swordfs 2>&1 | tail -1
echo ""

echo "==> Test 1: format volume"
sudo mkdir -p "$FORMAT_DIR"
if sudo "$SWORDFS" format --meta memory://local --volume testvol --volume-config-path "$FORMAT_DIR" 2>&1; then
  pass "format succeeded"
  if [ -f "$FORMAT_DIR/volume.json" ]; then
    pass "volume.json created"
    echo "  content: $(sudo cat "$FORMAT_DIR/volume.json")"
  else
    fail "volume.json not found"
  fi
else
  fail "format failed"
fi

echo ""
echo "==> Test 2: format again should fail (already exists)"
if sudo "$SWORDFS" format --meta memory://local --volume testvol --volume-config-path "$FORMAT_DIR" 2>&1; then
  fail "format should have failed on existing volume"
else
  pass "format correctly refused to overwrite"
fi

echo ""
echo "========================================="
echo "Results: $PASS passed, $FAIL failed"
echo "========================================="

if [ "$FAIL" -gt 0 ]; then
  exit 1
fi
exit 0
