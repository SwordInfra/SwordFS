#!/usr/bin/env bash
# Run the pinned upstream pjdfstest suite against a privileged SwordFS mount.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
VERSION_FILE="${PROJECT_DIR}/conformance/pjdfstest/version.env"
COMPOSE_FILE="${PROJECT_DIR}/docker-compose.e2e.yml"
# shellcheck source=scripts/testing/minio-test.sh
source "${PROJECT_DIR}/scripts/testing/minio-test.sh"

OUTPUT_DIR="${PROJECT_DIR}/build/pjdfstest-conformance"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ "$(id -u)" -ne 0 ]]; then
  echo "ERROR: pjdfstest conformance must run as root." >&2
  exit 2
fi
if [[ ! -r "${VERSION_FILE}" ]]; then
  echo "ERROR: missing ${VERSION_FILE}" >&2
  exit 2
fi

# shellcheck disable=SC1090
source "${VERSION_FILE}"
: "${PJDFSTEST_COMMIT:?PJDFSTEST_COMMIT must be set in version.env}"

SWORDFS_BIN="${SWORDFS_BIN:-${PROJECT_DIR}/build/swordfs}"
if [[ ! -x "${SWORDFS_BIN}" ]]; then
  echo "ERROR: SwordFS binary is not executable: ${SWORDFS_BIN}" >&2
  exit 2
fi

if docker compose version >/dev/null 2>&1; then
  DOCKER_COMPOSE=(docker compose)
else
  echo "ERROR: docker compose is required for pjdfstest conformance." >&2
  exit 2
fi

OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"
RAW_DIR="${OUTPUT_DIR}/raw"
rm -rf "${RAW_DIR}"
mkdir -p "${RAW_DIR}"

WORK_DIR="$(mktemp -d /tmp/swordfs-pjdfstest.XXXXXX)"
PJDFSTEST_DIR="${WORK_DIR}/pjdfstest"
MOUNTPOINT="${WORK_DIR}/mnt"
PIDFILE="${WORK_DIR}/swordfs.pid"
VOLUME="pjdfstest${GITHUB_RUN_ID:-$$}${GITHUB_RUN_ATTEMPT:-0}"
MOUNT_WORK="${MOUNTPOINT}/cases"

MINIO_PORT="${MINIO_PORT:-9000}"
S3_BUCKET="${S3_BUCKET:-swordfs-pjdfstest}"
MINIO_ROOT_USER="${MINIO_ROOT_USER:-minioadmin}"
MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD:-minioadmin}"
METADATA_URL="${SWORDFS_METADATA_URL:-redis://127.0.0.1:6379/14}"
BUCKET_URL="s3://127.0.0.1:${MINIO_PORT}/${S3_BUCKET}/${VOLUME}"
export MINIO_PORT S3_BUCKET MINIO_ROOT_USER MINIO_ROOT_PASSWORD

compose() {
  "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" "$@"
}

cleanup() {
  set +e
  if command -v findmnt >/dev/null 2>&1 && findmnt -T "${MOUNTPOINT}" >/dev/null 2>&1; then
    fusermount3 -u "${MOUNTPOINT}" >/dev/null 2>&1 || fusermount3 -uz "${MOUNTPOINT}" >/dev/null 2>&1 || true
  fi
  minio_test_stop
  compose down -v --remove-orphans >/dev/null 2>&1 || true
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

echo "=== Fetching pjdfstest ${PJDFSTEST_COMMIT} ==="
git init -q "${PJDFSTEST_DIR}"
git -C "${PJDFSTEST_DIR}" remote add origin https://github.com/pjd/pjdfstest.git
git -C "${PJDFSTEST_DIR}" fetch -q --depth 1 origin "${PJDFSTEST_COMMIT}"
git -C "${PJDFSTEST_DIR}" checkout -q --detach FETCH_HEAD
ACTUAL_PJDFSTEST_COMMIT="$(git -C "${PJDFSTEST_DIR}" rev-parse HEAD)"
if [[ "${ACTUAL_PJDFSTEST_COMMIT}" != "${PJDFSTEST_COMMIT}" ]]; then
  echo "ERROR: fetched pjdfstest commit ${ACTUAL_PJDFSTEST_COMMIT}, expected ${PJDFSTEST_COMMIT}" >&2
  exit 2
fi

echo "=== Building pjdfstest ==="
(
  cd "${PJDFSTEST_DIR}"
  autoreconf -ifs
  ./configure
  make -j2 pjdfstest
) >"${OUTPUT_DIR}/pjdfstest-build.log" 2>&1

echo "=== Starting Redis and MinIO ==="
compose up -d --wait redis >"${OUTPUT_DIR}/dependencies.log" 2>&1
minio_test_start "${OUTPUT_DIR}/minio"
minio_test_create_bucket "${S3_BUCKET}" >>"${OUTPUT_DIR}/dependencies.log" 2>&1
minio_test_print_versions >>"${OUTPUT_DIR}/dependencies.log" 2>&1

mkdir -p "${MOUNTPOINT}"
echo "=== Formatting SwordFS conformance volume ==="
SWORDFS_S3_NO_SSL=1 \
AWS_DEFAULT_REGION=auto \
AWS_ACCESS_KEY_ID="${MINIO_ROOT_USER}" \
AWS_SECRET_ACCESS_KEY="${MINIO_ROOT_PASSWORD}" \
"${SWORDFS_BIN}" --log-file "${OUTPUT_DIR}/swordfs.log" format \
  --volume "${VOLUME}" \
  --meta "${METADATA_URL}" \
  --bucket "${BUCKET_URL}"

echo "=== Mounting SwordFS for pjdfstest ==="
SWORDFS_S3_NO_SSL=1 \
AWS_DEFAULT_REGION=auto \
AWS_ACCESS_KEY_ID="${MINIO_ROOT_USER}" \
AWS_SECRET_ACCESS_KEY="${MINIO_ROOT_PASSWORD}" \
"${SWORDFS_BIN}" --log-file "${OUTPUT_DIR}/swordfs.log" mount \
  --volume "${VOLUME}" \
  --meta "${METADATA_URL}" \
  --fuse-threads 4 \
  --storage-thread-count 4 \
  --pidfile "${PIDFILE}" \
  -o allow_other \
  "${MOUNTPOINT}"

for _ in $(seq 1 100); do
  if findmnt -T "${MOUNTPOINT}" -n -o FSTYPE 2>/dev/null | grep -qi fuse; then
    break
  fi
  sleep 0.05
done
if ! findmnt -T "${MOUNTPOINT}" -n -o FSTYPE 2>/dev/null | grep -qi fuse; then
  echo "ERROR: SwordFS mount did not become ready at ${MOUNTPOINT}" >&2
  exit 2
fi

mkdir -p "${MOUNT_WORK}"

python3 - "${OUTPUT_DIR}/environment.json" <<'PY'
import json
import os
import pathlib
import platform
import subprocess
import sys

def command(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip().splitlines()[0]
    except Exception:
        return "unknown"

path = pathlib.Path(sys.argv[1])
path.write_text(
    json.dumps(
        {
            "os": platform.platform(),
            "kernel": platform.release(),
            "libfuse": command("fusermount3", "--version"),
            "runner": os.environ.get("RUNNER_OS", "local"),
        },
        indent=2,
        sort_keys=True,
    )
    + "\n",
    encoding="utf-8",
)
PY

mapfile -t TESTS < <(find "${PJDFSTEST_DIR}/tests" -type f -name '*.t' | sort)
if [[ "${#TESTS[@]}" -eq 0 ]]; then
  echo "ERROR: pinned pjdfstest revision contains no tests." >&2
  exit 2
fi

echo "=== Running ${#TESTS[@]} pjdfstest scripts ==="
index=0
for test_path in "${TESTS[@]}"; do
  index=$((index + 1))
  relative="${test_path#${PJDFSTEST_DIR}/}"
  tap_path="${RAW_DIR}/${relative}.tap"
  exit_path="${RAW_DIR}/${relative}.exit"
  case_dir="${MOUNT_WORK}/${index}"
  mkdir -p "$(dirname "${tap_path}")" "${case_dir}"

  echo "[${index}/${#TESTS[@]}] ${relative}"
  set +e
  (
    cd "${case_dir}"
    timeout --kill-after=10s 120s sh "${test_path}"
  ) >"${tap_path}" 2>&1
  status=$?
  set -e
  printf '%s\n' "${status}" >"${exit_path}"
done

echo "=== pjdfstest raw execution complete ==="
