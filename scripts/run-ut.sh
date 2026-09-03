#!/usr/bin/env bash
# ────────────────────────────────────────────────────────────────
# run-ut.sh — Run SwordFS unit tests.
#
# Starts the Redis dependency with Docker Compose, runs the pre-built
# unit test suite, and cleans up the dependency. All extra arguments
# are forwarded to the test binary.
# ────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

UNIT_TEST_BIN="${UNIT_TEST_BIN:-${PROJECT_DIR}/build/swordfs_test}"
COMPOSE_FILE="${PROJECT_DIR}/docker-compose.e2e.yml"
SWORDFS_REDIS_TEST_URL="redis://127.0.0.1:6379"

if docker compose version >/dev/null 2>&1; then
  DOCKER_COMPOSE=(docker compose)
elif sudo docker compose version >/dev/null 2>&1; then
  DOCKER_COMPOSE=(sudo docker compose)
else
  echo "ERROR: docker compose is required to run unit tests."
  exit 1
fi

compose() {
  "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" "$@"
}

cleanup() {
  echo "=== Stopping unit test Redis ==="
  compose down -v --remove-orphans
}
trap cleanup EXIT

echo "=== Starting unit test Redis ==="
compose up -d --wait redis

echo "=== Running unit tests ==="
cd "${PROJECT_DIR}"
SWORDFS_REDIS_TEST_URL="${SWORDFS_REDIS_TEST_URL}" \
  "${UNIT_TEST_BIN}" "$@"
