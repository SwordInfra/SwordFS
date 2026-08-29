#!/usr/bin/env bash
# ────────────────────────────────────────────────────────────────
# run-ut.sh — Run SwordFS unit tests.
#
# Starts the Redis dependency required by Redis-backed unit tests,
# runs the pre-built unit test suite, and cleans up the dependency.
# All extra arguments are forwarded to the test binary.
# ────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

UNIT_TEST_BIN="${UNIT_TEST_BIN:-${PROJECT_DIR}/build/swordfs_test}"
REDIS_CONTAINER="swordfs-unit-redis"
REDIS_IMAGE="${REDIS_IMAGE:-redis:7-alpine}"
REDIS_PORT="6379"
SWORDFS_REDIS_TEST_URL="redis://127.0.0.1:${REDIS_PORT}"

cleanup() {
  echo "=== Stopping unit test Redis ==="
  docker rm -f "${REDIS_CONTAINER}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "=== Starting unit test Redis ==="
docker run -d --name "${REDIS_CONTAINER}" \
  -p "${REDIS_PORT}:6379" \
  "${REDIS_IMAGE}" >/dev/null

until docker exec "${REDIS_CONTAINER}" redis-cli ping | grep -q PONG; do
  sleep 1
done

echo "=== Running unit tests ==="
cd "${PROJECT_DIR}"
SWORDFS_REDIS_TEST_URL="${SWORDFS_REDIS_TEST_URL}" \
  "${UNIT_TEST_BIN}" "$@"
