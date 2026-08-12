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

# Auto-detect docker command (fall back to sudo if needed).
if docker ps >/dev/null 2>&1; then
  DOCKER="docker"
else
  DOCKER="sudo docker"
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

# ── MinIO ────────────────────────────────────────────────────────

start_minio() {
  echo "=== Starting MinIO container (${MINIO_CONTAINER}) ==="
  ${DOCKER} rm -f "${MINIO_CONTAINER}" 2>/dev/null || true
  ${DOCKER} run -d --name "${MINIO_CONTAINER}" \
    -p "${MINIO_ENDPOINT##*:}:${MINIO_ENDPOINT##*:}" \
    -e MINIO_ROOT_USER="${MINIO_ROOT_USER}" \
    -e MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD}" \
    minio/minio:latest \
    server /data --address ":${MINIO_ENDPOINT##*:}"

  echo "=== Waiting for MinIO ==="
  for i in $(seq 1 30); do
    if curl -sf "http://${MINIO_ENDPOINT}/minio/health/live" >/dev/null 2>&1; then
      echo "MinIO is ready."
      return 0
    fi
    echo "  ... waiting (${i}/30)"
    sleep 2
  done
  echo "ERROR: MinIO did not become healthy."
  return 1
}

stop_minio() {
  echo "=== Stopping MinIO container ==="
  ${DOCKER} rm -f "${MINIO_CONTAINER}" 2>/dev/null || true
}

create_bucket() {
  echo "=== Creating bucket ${S3_BUCKET} ==="
  ${DOCKER} exec "${MINIO_CONTAINER}" \
    mc alias set local http://localhost:9000 \
      "${MINIO_ROOT_USER}" "${MINIO_ROOT_PASSWORD}"
  ${DOCKER} exec "${MINIO_CONTAINER}" \
    mc mb "local/${S3_BUCKET}" --ignore-existing
}

# ── Cleanup ──────────────────────────────────────────────────────

cleanup() {
  stop_minio
}
trap cleanup EXIT

# ── Main ─────────────────────────────────────────────────────────

setup_fuse
build
start_minio
create_bucket

echo "=== Running E2E tests ==="
cd "$PROJECT_DIR"

SWORDFS_S3_NO_SSL=1 \
SWORDFS_E2E_S3_BUCKET="s3://${MINIO_ENDPOINT}/${S3_BUCKET}" \
SWORDFS_BIN="${SWORDFS_BIN}" \
AWS_DEFAULT_REGION=auto \
AWS_ACCESS_KEY_ID="${MINIO_ROOT_USER}" \
AWS_SECRET_ACCESS_KEY="${MINIO_ROOT_PASSWORD}" \
"${E2E_BIN}" "$@"
