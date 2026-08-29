#!/usr/bin/env bash
# ────────────────────────────────────────────────────────────────
# run-e2e.sh — Run SwordFS end-to-end tests.
#
# Builds swordfs, starts a local MinIO container, creates the test
# bucket, and runs the E2E test suite.  All extra arguments are
# forwarded to the test binary.
#
# Usage:
#   ./scripts/run-e2e.sh
#   ./scripts/run-e2e.sh --gtest_filter='BasicOpsTest.*'
# ────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

# ── Defaults ────────────────────────────────────────────────────

SWORDFS_BIN="${SWORDFS_BIN:-${PROJECT_DIR}/build/swordfs}"
E2E_BIN="${E2E_BIN:-${PROJECT_DIR}/build/swordfs_e2e_test}"
MINIO_ENDPOINT="${MINIO_ENDPOINT:-127.0.0.1:9000}"
S3_BUCKET="${S3_BUCKET:-swordfs-e2e}"
MINIO_ROOT_USER="${MINIO_ROOT_USER:-minioadmin}"
MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD:-minioadmin}"
MINIO_CONTAINER="${MINIO_CONTAINER:-swordfs-e2e-minio}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"
REDIS_CONTAINER="${REDIS_CONTAINER:-swordfs-e2e-redis}"
REDIS_IMAGE="${REDIS_IMAGE:-redis:7-alpine}"
SWORDFS_METADATA_URL="redis://${REDIS_HOST}:${REDIS_PORT}/15"

# Auto-detect docker command (fall back to sudo if needed).
if docker ps >/dev/null 2>&1; then
  DOCKER=(docker)
else
  DOCKER=(sudo docker)
fi

# ── FUSE setup ───────────────────────────────────────────────────

setup_fuse() {
  if ! lsmod 2>/dev/null | grep -q '^fuse '; then
    echo "=== Loading FUSE kernel module ==="
    sudo modprobe fuse
  fi
  if [ -e /dev/fuse ]; then
    sudo chmod 666 /dev/fuse
  fi
}

# ── Build ────────────────────────────────────────────────────────

build() {
  echo "=== Building SwordFS ==="
  cd "$PROJECT_DIR"
  cmake --preset default
  cmake --build build --target swordfs swordfs_e2e_test -j2
}

# ── Container helpers ────────────────────────────────────────────

remove_container() {
  local container="$1"
  local volumes="${2:-false}"
  echo "=== Stopping ${container} ==="
  if [[ "${volumes}" == true ]]; then
    "${DOCKER[@]}" rm -f -v "${container}" 2>/dev/null || true
  else
    "${DOCKER[@]}" rm -f "${container}" 2>/dev/null || true
  fi
}

wait_for_container() {
  local container="$1"
  local retries="$2"
  local delay="$3"
  shift 3

  for i in $(seq 1 "${retries}"); do
    if "$@"; then
      echo "${container} is ready."
      return 0
    fi
    echo "  ... waiting (${i}/${retries})"
    sleep "${delay}"
  done
  echo "ERROR: ${container} did not become healthy."
  return 1
}

# ── MinIO ────────────────────────────────────────────────────────

start_minio() {
  echo "=== Starting MinIO container (${MINIO_CONTAINER}) ==="
  remove_container "${MINIO_CONTAINER}" true
  "${DOCKER[@]}" run -d --name "${MINIO_CONTAINER}" \
    -p "${MINIO_ENDPOINT##*:}:${MINIO_ENDPOINT##*:}" \
    -e MINIO_ROOT_USER="${MINIO_ROOT_USER}" \
    -e MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD}" \
    minio/minio:latest \
    server /data --address ":${MINIO_ENDPOINT##*:}"

  echo "=== Waiting for MinIO ==="
  wait_for_container "MinIO" 30 2 \
    curl -sf "http://${MINIO_ENDPOINT}/minio/health/live"
}

start_redis() {
  echo "=== Starting Redis container (${REDIS_CONTAINER}) ==="
  remove_container "${REDIS_CONTAINER}"
  "${DOCKER[@]}" run -d --name "${REDIS_CONTAINER}" \
    -p "${REDIS_HOST}:${REDIS_PORT}:6379" \
    "${REDIS_IMAGE}"

  echo "=== Waiting for Redis ==="
  wait_for_container "Redis" 30 1 \
    "${DOCKER[@]}" exec "${REDIS_CONTAINER}" redis-cli ping
}

create_bucket() {
  echo "=== Creating bucket ${S3_BUCKET} ==="
  "${DOCKER[@]}" exec "${MINIO_CONTAINER}" \
    mc alias set local http://localhost:9000 \
      "${MINIO_ROOT_USER}" "${MINIO_ROOT_PASSWORD}"
  "${DOCKER[@]}" exec "${MINIO_CONTAINER}" \
    mc mb "local/${S3_BUCKET}" --ignore-existing
}

# ── Cleanup ──────────────────────────────────────────────────────

cleanup() {
  remove_container "${REDIS_CONTAINER}"
  remove_container "${MINIO_CONTAINER}" true
}
trap cleanup EXIT

# ── Main ─────────────────────────────────────────────────────────

# Skip FUSE setup (needs sudo) when the module is already available,
# e.g. CI environments: SKIP_FUSE_SETUP=1 ./scripts/run-e2e.sh
if [ -z "${SKIP_FUSE_SETUP:-}" ]; then
  setup_fuse
fi
build
start_minio
create_bucket

start_redis

echo "=== Running E2E tests ==="
cd "$PROJECT_DIR"

SWORDFS_S3_NO_SSL=1 \
SWORDFS_E2E_S3_BUCKET="s3://${MINIO_ENDPOINT}/${S3_BUCKET}" \
SWORDFS_METADATA_URL="${SWORDFS_METADATA_URL}" \
SWORDFS_BIN="${SWORDFS_BIN}" \
AWS_DEFAULT_REGION=auto \
AWS_ACCESS_KEY_ID="${MINIO_ROOT_USER}" \
AWS_SECRET_ACCESS_KEY="${MINIO_ROOT_PASSWORD}" \
"${E2E_BIN}" "$@"
