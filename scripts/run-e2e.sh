#!/usr/bin/env bash
# ────────────────────────────────────────────────────────────────
# run-e2e.sh — Run SwordFS end-to-end tests.
#
# Starts the Redis and MinIO dependencies with Docker Compose,
# creates the test bucket, and runs the pre-built E2E suite.
# All extra arguments are forwarded to the test binary.
# ────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
COMPOSE_FILE="${PROJECT_DIR}/docker-compose.e2e.yml"

SWORDFS_BIN="${SWORDFS_BIN:-${PROJECT_DIR}/build/swordfs}"
E2E_BIN="${E2E_BIN:-${PROJECT_DIR}/build/swordfs_e2e_test}"
MINIO_PORT="${MINIO_PORT:-9000}"
MINIO_ENDPOINT="127.0.0.1:${MINIO_PORT}"
S3_BUCKET="${S3_BUCKET:-swordfs-e2e}"
MINIO_ROOT_USER="${MINIO_ROOT_USER:-minioadmin}"
MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD:-minioadmin}"
SWORDFS_METADATA_URL="redis://127.0.0.1:6379/15"
export MINIO_PORT S3_BUCKET MINIO_ROOT_USER MINIO_ROOT_PASSWORD

if docker compose version >/dev/null 2>&1; then
  DOCKER_COMPOSE=(docker compose)
elif sudo docker compose version >/dev/null 2>&1; then
  DOCKER_COMPOSE=(sudo docker compose)
else
  echo "ERROR: docker compose is required to run E2E tests."
  exit 1
fi

compose() {
  "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" "$@"
}

setup_fuse() {
  if ! lsmod 2>/dev/null | grep -q '^fuse '; then
    echo "=== Loading FUSE kernel module ==="
    sudo modprobe fuse
  fi
  if [ -e /dev/fuse ]; then
    sudo chmod 666 /dev/fuse
  fi
}

start_dependencies() {
  echo "=== Starting E2E dependencies ==="
  compose up -d --wait
}

create_bucket() {
  echo "=== Creating bucket ${S3_BUCKET} ==="
  compose exec -T minio \
    mc alias set local http://localhost:9000 \
      "${MINIO_ROOT_USER}" "${MINIO_ROOT_PASSWORD}"
  compose exec -T minio \
    mc mb "local/${S3_BUCKET}" --ignore-existing
}

cleanup() {
  echo "=== Stopping E2E dependencies ==="
  compose down -v --remove-orphans
}
trap cleanup EXIT

if [ -z "${SKIP_FUSE_SETUP:-}" ]; then
  setup_fuse
fi

start_dependencies
create_bucket

echo "=== Running E2E tests ==="
cd "${PROJECT_DIR}"

SWORDFS_S3_NO_SSL=1 \
SWORDFS_E2E_S3_BUCKET="s3://${MINIO_ENDPOINT}/${S3_BUCKET}" \
SWORDFS_METADATA_URL="${SWORDFS_METADATA_URL}" \
SWORDFS_BIN="${SWORDFS_BIN}" \
AWS_DEFAULT_REGION=auto \
AWS_ACCESS_KEY_ID="${MINIO_ROOT_USER}" \
AWS_SECRET_ACCESS_KEY="${MINIO_ROOT_PASSWORD}" \
"${E2E_BIN}" "$@"
