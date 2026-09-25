#!/usr/bin/env bash
# Run pinned upstream fstests against SwordFS' Redis + MinIO FUSE path.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
VERSION_FILE="${PROJECT_DIR}/conformance/fstests/version.env"
PLAN_SCRIPT="${SCRIPT_DIR}/plan.py"
COMPOSE_FILE="${PROJECT_DIR}/docker-compose.e2e.yml"
# shellcheck source=scripts/testing/minio-test.sh
source "${PROJECT_DIR}/scripts/testing/minio-test.sh"
MOUNT_HELPER_SOURCE="${SCRIPT_DIR}/mount-helper.sh"
MOUNT_HELPER_TARGET="/sbin/mount.fuse.swordfs"
XUNIT_MERGER="${SCRIPT_DIR}/xunit_merge.py"
MOUNT_CONFIG="/tmp/swordfs-fstests-mount.env"

parse_timeout_seconds() {
  local name="$1"
  local value="$2"
  if [[ ! "${value}" =~ ^([1-9][0-9]*)([smh])$ ]]; then
    echo "ERROR: unsupported ${name}: ${value}" >&2
    return 2
  fi
  local amount="${BASH_REMATCH[1]}"
  local unit="${BASH_REMATCH[2]}"
  local scale
  case "${unit}" in
    s) scale=1 ;;
    m) scale=60 ;;
    h) scale=3600 ;;
  esac
  echo $((amount * scale))
}

# A stuck FUSE request can also stall unmount. Keep recovery bounded so the
# per-test deadline cannot turn into another unbounded wait in isolation.
bounded_fusermount() {
  local mode="$1"
  local mountpoint="$2"
  timeout --foreground --kill-after=5s 10s fusermount3 "${mode}" "${mountpoint}" >/dev/null 2>&1
}

OUTPUT_DIR="${PROJECT_DIR}/build/fstests-conformance"
TESTS_FILE=""
SHARD_NAME=""
SUITE_TIMEOUT_OVERRIDE=""
TEST_TIMEOUT_OVERRIDE=""
VERIFY_SELECTION=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --tests-file)
      TESTS_FILE="$2"
      shift 2
      ;;
    --shard-name)
      SHARD_NAME="$2"
      shift 2
      ;;
    --suite-timeout)
      SUITE_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --test-timeout)
      TEST_TIMEOUT_OVERRIDE="$2"
      shift 2
      ;;
    --verify-selection)
      VERIFY_SELECTION=1
      shift
      ;;
    *)
      echo "ERROR: unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

if [[ -z "${TESTS_FILE}" || ! -r "${TESTS_FILE}" ]]; then
  echo "ERROR: --tests-file must name a readable shard testcase list" >&2
  exit 2
fi
if [[ -z "${SHARD_NAME}" ]]; then
  echo "ERROR: --shard-name is required" >&2
  exit 2
fi

if [[ "$(id -u)" -ne 0 ]]; then
  echo "ERROR: fstests conformance must run as root." >&2
  exit 2
fi
if [[ ! -r "${VERSION_FILE}" ]]; then
  echo "ERROR: missing ${VERSION_FILE}" >&2
  exit 2
fi

# shellcheck disable=SC1090
source "${VERSION_FILE}"
if [[ -n "${SUITE_TIMEOUT_OVERRIDE}" ]]; then
  FSTESTS_SUITE_TIMEOUT="${SUITE_TIMEOUT_OVERRIDE}"
fi
if [[ -n "${TEST_TIMEOUT_OVERRIDE}" ]]; then
  FSTESTS_TEST_TIMEOUT="${TEST_TIMEOUT_OVERRIDE}"
fi
: "${FSTESTS_REPOSITORY:?FSTESTS_REPOSITORY must be set}"
: "${FSTESTS_COMMIT:?FSTESTS_COMMIT must be set}"
: "${FSTESTS_GROUP:?FSTESTS_GROUP must be set}"
: "${FSTESTS_SUITE_TIMEOUT:?FSTESTS_SUITE_TIMEOUT must be set}"
: "${FSTESTS_TEST_TIMEOUT:?FSTESTS_TEST_TIMEOUT must be set}"
: "${LIBURING_REPOSITORY:?LIBURING_REPOSITORY must be set}"
: "${LIBURING_COMMIT:?LIBURING_COMMIT must be set}"

SWORDFS_BIN="${SWORDFS_BIN:-${PROJECT_DIR}/build/swordfs}"
if [[ ! -x "${SWORDFS_BIN}" ]]; then
  echo "ERROR: SwordFS binary is not executable: ${SWORDFS_BIN}" >&2
  exit 2
fi
if docker compose version >/dev/null 2>&1; then
  DOCKER_COMPOSE=(docker compose)
else
  echo "ERROR: docker compose is required for fstests conformance." >&2
  exit 2
fi

OUTPUT_DIR="$(mkdir -p "${OUTPUT_DIR}" && cd "${OUTPUT_DIR}" && pwd)"
RAW_DIR="${OUTPUT_DIR}/raw"
rm -rf "${RAW_DIR}"
mkdir -p "${RAW_DIR}"

python3 - "${TESTS_FILE}" "${OUTPUT_DIR}/planned-tests.txt" "${OUTPUT_DIR}/shard.json" "${SHARD_NAME}" <<'PY'
import json
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1])
planned_path = pathlib.Path(sys.argv[2])
metadata_path = pathlib.Path(sys.argv[3])
shard = sys.argv[4]
test_re = re.compile(r"^[a-z0-9_-]+/[0-9]+$")
tests = []
seen = set()
for line_number, raw in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
    test = raw.strip()
    if not test:
        continue
    if not test_re.fullmatch(test):
        raise SystemExit(f"{source}:{line_number}: invalid testcase {test!r}")
    if test in seen:
        raise SystemExit(f"{source}:{line_number}: duplicate testcase {test}")
    seen.add(test)
    tests.append(test)
if not tests:
    raise SystemExit(f"{source}: shard testcase list is empty")
planned_path.write_text("".join(f"{test}\n" for test in tests), encoding="utf-8")
metadata_path.write_text(
    json.dumps({"shard": shard, "planned_tests": tests}, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY

python3 - "${OUTPUT_DIR}/environment.json" <<PY
import json
import os
import pathlib
import platform
import subprocess

def command(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT).strip().splitlines()[0]
    except Exception:
        return "unknown"

pathlib.Path("${OUTPUT_DIR}/environment.json").write_text(
    json.dumps(
        {
            "os": platform.platform(),
            "kernel": platform.release(),
            "libfuse": command("fusermount3", "--version"),
            "runner": os.environ.get("RUNNER_OS", "local"),
            "backend": "redis+minio-s3",
            "fstests_repository": "${FSTESTS_REPOSITORY}",
            "fstests_selector": "${FSTESTS_GROUP}",
            "fstests_suite_timeout": "${FSTESTS_SUITE_TIMEOUT}",
            "fstests_test_timeout": "${FSTESTS_TEST_TIMEOUT}",
            "liburing_commit": "${LIBURING_COMMIT}",
            "shard": "${SHARD_NAME}",
        },
        indent=2,
        sort_keys=True,
    ) + "\n",
    encoding="utf-8",
)
PY

# Do not place the random suffix after a dot. Upstream xfstests dependency
# generation rewrites `.o` targets with sed; a random suffix beginning with
# `o` would therefore make a path such as `swordfs-fstests.oXXXXX` look like
# an object-file token and can corrupt absolute dependency paths.
WORK_DIR="$(mktemp -d /tmp/swordfs-fstests-XXXXXX)"
# mktemp creates mode 0700 by default. Upstream fstests deliberately executes
# a subset of cases as fsgqa/other non-root identities, so those users must be
# able to traverse the harness parent directory before they can reach the FUSE
# TEST/SCRATCH mounts. Keep write ownership with root while allowing traversal.
chmod 0755 "${WORK_DIR}"
FSTESTS_DIR="${WORK_DIR}/xfstests"
LIBURING_DIR="${WORK_DIR}/liburing"
LIBURING_PREFIX="${WORK_DIR}/liburing-install"
TEST_DIR="${WORK_DIR}/test"
SCRATCH_MNT="${WORK_DIR}/scratch"
SELECT_RESULTS="${RAW_DIR}/selection-verification"
RUN_RESULTS="${RAW_DIR}/results"
XUNIT_PARTS="${RAW_DIR}/xunit-parts"
RUN_TAG="${GITHUB_RUN_ID:-$$}${GITHUB_RUN_ATTEMPT:-0}"
TEST_VOLUME="fsteststest${RUN_TAG}"
SCRATCH_VOLUME="fstestsscratch${RUN_TAG}"
# fstests unmounts FUSE filesystems through TEST_DEV/SCRATCH_DEV.  Make each
# FUSE source identity the mountpoint itself: FSTYP=fuse permits opaque device
# strings, and this makes `umount $TEST_DEV` / `umount $SCRATCH_DEV` resolve
# unambiguously as mountpoint-based unmounts while still giving TEST/SCRATCH
# distinct identities for findmnt/df checks.
TEST_DEV="${TEST_DIR}"
SCRATCH_DEV="${SCRATCH_MNT}"

MINIO_PORT="${MINIO_PORT:-9000}"
S3_BUCKET="${S3_BUCKET:-swordfs-fstests}"
MINIO_ROOT_USER="${MINIO_ROOT_USER:-minioadmin}"
MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD:-minioadmin}"
METADATA_URL="${SWORDFS_METADATA_URL:-redis://127.0.0.1:6379/13}"
TEST_BUCKET_URL="s3://127.0.0.1:${MINIO_PORT}/${S3_BUCKET}/${TEST_VOLUME}"
SCRATCH_BUCKET_URL="s3://127.0.0.1:${MINIO_PORT}/${S3_BUCKET}/${SCRATCH_VOLUME}"
export MINIO_PORT S3_BUCKET MINIO_ROOT_USER MINIO_ROOT_PASSWORD

compose() {
  "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" "$@"
}

capture_diagnostics() {
  set +e
  dmesg >"${OUTPUT_DIR}/dmesg.txt" 2>&1 || true
  findmnt >"${OUTPUT_DIR}/findmnt.txt" 2>&1 || true
}

capture_backend_diagnostics() {
  set +e
  {
    echo "=== Backend runtime state ==="
    compose ps -a
    for service in redis; do
      container_id="$(compose ps -a -q "${service}" 2>/dev/null)"
      if [[ -n "${container_id}" ]]; then
        echo "=== ${service} container state ==="
        docker inspect --format '{{json .State}}' "${container_id}" || true
        printf '%s restart-count: ' "${service}"
        docker inspect --format '{{.RestartCount}}' "${container_id}" || true
      fi
    done
    echo "=== Redis runtime diagnostics ==="
    timeout 10s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T redis redis-cli INFO clients || true
    timeout 10s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T redis redis-cli INFO stats || true
    timeout 10s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T redis redis-cli CONFIG GET timeout || true
    timeout 10s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T redis redis-cli CONFIG GET tcp-keepalive || true
    timeout 10s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T redis redis-cli CONFIG GET maxclients || true
    timeout 10s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T redis redis-cli CONFIG GET client-output-buffer-limit || true
    echo "=== Redis logs ==="
    timeout 20s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" logs --no-color redis || true
    echo "=== MinIO log ==="
    cat "${MINIO_TEST_LOG:-/dev/null}" 2>/dev/null || true
  } >"${OUTPUT_DIR}/dependencies-runtime.log" 2>&1
}

cleanup() {
  set +e
  capture_diagnostics
  capture_backend_diagnostics
  for mountpoint in "${TEST_DIR}" "${SCRATCH_MNT}"; do
    if findmnt -T "${mountpoint}" -n -o FSTYPE 2>/dev/null | grep -qi fuse; then
      bounded_fusermount -u "${mountpoint}" || \
        bounded_fusermount -uz "${mountpoint}" || true
    fi
  done
  minio_test_stop
  compose down -v --remove-orphans >/dev/null 2>&1 || true
  if [[ -L "${MOUNT_HELPER_TARGET}" ]] && \
     [[ "$(readlink -f "${MOUNT_HELPER_TARGET}")" == "$(readlink -f "${MOUNT_HELPER_SOURCE}")" ]]; then
    rm -f "${MOUNT_HELPER_TARGET}"
  fi
  rm -f "${MOUNT_CONFIG}"
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

echo "=== Fetching liburing ${LIBURING_COMMIT} ==="
git init -q "${LIBURING_DIR}"
git -C "${LIBURING_DIR}" remote add origin "${LIBURING_REPOSITORY}"
git -C "${LIBURING_DIR}" fetch -q --depth 1 origin "${LIBURING_COMMIT}"
git -C "${LIBURING_DIR}" checkout -q --detach FETCH_HEAD
ACTUAL_LIBURING_COMMIT="$(git -C "${LIBURING_DIR}" rev-parse HEAD)"
if [[ "${ACTUAL_LIBURING_COMMIT}" != "${LIBURING_COMMIT}" ]]; then
  echo "ERROR: fetched liburing ${ACTUAL_LIBURING_COMMIT}, expected ${LIBURING_COMMIT}" >&2
  exit 2
fi

# Build liburing against the runner's actual UAPI headers. Ubuntu's hosted
# runner package can ship a generated compat.h that expects a kernel header
# absent from the image, while installing matching kernel headers mutates the
# host package graph. A pinned source build preserves io_uring test coverage
# without making the conformance environment depend on that packaging mismatch.
echo "=== Building pinned liburing ==="
(
  cd "${LIBURING_DIR}"
  ./configure --prefix="${LIBURING_PREFIX}"
  jobs="$(nproc)"
  jobs=$((jobs / 2))
  ((jobs > 0)) || jobs=1
  make -j"${jobs}"
  make install
) >"${OUTPUT_DIR}/liburing-build.log" 2>&1
export PKG_CONFIG_PATH="${LIBURING_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
export CPPFLAGS="-I${LIBURING_PREFIX}/include${CPPFLAGS:+ ${CPPFLAGS}}"
export LDFLAGS="-L${LIBURING_PREFIX}/lib${LDFLAGS:+ ${LDFLAGS}}"
export LIBRARY_PATH="${LIBURING_PREFIX}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export LD_LIBRARY_PATH="${LIBURING_PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

echo "=== Fetching fstests ${FSTESTS_COMMIT} ==="
git init -q "${FSTESTS_DIR}"
git -C "${FSTESTS_DIR}" remote add origin "${FSTESTS_REPOSITORY}"
git -C "${FSTESTS_DIR}" fetch -q --depth 1 origin "${FSTESTS_COMMIT}"
git -C "${FSTESTS_DIR}" checkout -q --detach FETCH_HEAD
ACTUAL_FSTESTS_COMMIT="$(git -C "${FSTESTS_DIR}" rev-parse HEAD)"
if [[ "${ACTUAL_FSTESTS_COMMIT}" != "${FSTESTS_COMMIT}" ]]; then
  echo "ERROR: fetched fstests ${ACTUAL_FSTESTS_COMMIT}, expected ${FSTESTS_COMMIT}" >&2
  exit 2
fi

echo "=== Building fstests ==="
(
  cd "${FSTESTS_DIR}"
  make configure
  ./configure
  jobs="$(nproc)"
  jobs=$((jobs / 2))
  ((jobs > 0)) || jobs=1
  make -j"${jobs}"
) >"${OUTPUT_DIR}/fstests-build.log" 2>&1

echo "=== Ensuring fstests users and groups ==="
getent group fsgqa >/dev/null 2>&1 || groupadd fsgqa
id fsgqa >/dev/null 2>&1 || useradd -m -g fsgqa fsgqa

getent group fsgqa2 >/dev/null 2>&1 || groupadd fsgqa2
id fsgqa2 >/dev/null 2>&1 || useradd -m -g fsgqa2 fsgqa2

# generic/381 requires both a user and group whose name starts with digits.
# useradd's --badname applies the relaxed name validation while -U creates the
# matching primary group atomically on a fresh CI runner.
if ! id 123456-fsgqa >/dev/null 2>&1; then
  useradd --badname -m -U 123456-fsgqa
fi
if ! getent group 123456-fsgqa >/dev/null 2>&1; then
  echo "ERROR: fstests requires group 123456-fsgqa" >&2
  exit 2
fi

if ! runuser -u fsgqa -- test -x "${WORK_DIR}"; then
  echo "ERROR: fstests work directory is not traversable by fsgqa: ${WORK_DIR}" >&2
  exit 2
fi

echo "=== Starting Redis and MinIO ==="
compose up -d --wait redis >"${OUTPUT_DIR}/dependencies.log" 2>&1
minio_test_start "${OUTPUT_DIR}/minio"
minio_test_create_bucket "${S3_BUCKET}" >>"${OUTPUT_DIR}/dependencies.log" 2>&1

# Preserve the concrete Redis image and pinned MinIO tool versions used by this
# run so conformance evidence remains diagnosable across dependency changes.
{
  echo "=== Backend image identities ==="
  compose images
  for service in redis; do
    container_id="$(compose ps -q "${service}")"
    if [[ -n "${container_id}" ]]; then
      printf '%s image-id: ' "${service}"
      docker inspect --format '{{.Image}}' "${container_id}" || true
    fi
  done
  echo "=== Backend versions ==="
  compose exec -T redis redis-server --version || true
  minio_test_print_versions || true
} >>"${OUTPUT_DIR}/dependencies.log" 2>&1

format_volume() {
  local volume="$1"
  local bucket_url="$2"
  SWORDFS_S3_NO_SSL=1 \
  AWS_DEFAULT_REGION=auto \
  AWS_ACCESS_KEY_ID="${MINIO_ROOT_USER}" \
  AWS_SECRET_ACCESS_KEY="${MINIO_ROOT_PASSWORD}" \
  "${SWORDFS_BIN}" --log-file "${OUTPUT_DIR}/swordfs-format.log" format \
    --volume "${volume}" \
    --meta "${METADATA_URL}" \
    --bucket "${bucket_url}"
}

echo "=== Formatting fresh SwordFS fstests volumes ==="
format_volume "${TEST_VOLUME}" "${TEST_BUCKET_URL}"
format_volume "${SCRATCH_VOLUME}" "${SCRATCH_BUCKET_URL}"
mkdir -p "${TEST_DIR}" "${SCRATCH_MNT}"

write_mount_config() {
  : >"${MOUNT_CONFIG}"
  printf 'FSTESTS_SWORDFS_BIN=%q\n' "${SWORDFS_BIN}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_METADATA_URL=%q\n' "${METADATA_URL}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_TEST_DEV=%q\n' "${TEST_DEV}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_TEST_VOLUME=%q\n' "${TEST_VOLUME}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_SCRATCH_DEV=%q\n' "${SCRATCH_DEV}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_SCRATCH_VOLUME=%q\n' "${SCRATCH_VOLUME}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_MINIO_ROOT_USER=%q\n' "${MINIO_ROOT_USER}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_MINIO_ROOT_PASSWORD=%q\n' "${MINIO_ROOT_PASSWORD}" >>"${MOUNT_CONFIG}"
  printf 'FSTESTS_LOG_DIR=%q\n' "${OUTPUT_DIR}" >>"${MOUNT_CONFIG}"
  chmod 600 "${MOUNT_CONFIG}"
}
write_mount_config

if [[ -e "${MOUNT_HELPER_TARGET}" || -L "${MOUNT_HELPER_TARGET}" ]]; then
  echo "ERROR: refusing to replace existing ${MOUNT_HELPER_TARGET}" >&2
  exit 2
fi
ln -s "${MOUNT_HELPER_SOURCE}" "${MOUNT_HELPER_TARGET}"

wait_for_daemon_exit() {
  local pid="$1"
  local state=""
  for _ in $(seq 1 200); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      return 0
    fi
    if [[ -r "/proc/${pid}/stat" ]]; then
      state="$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)"
      [[ "${state}" == "Z" ]] && return 0
    fi
    sleep 0.05
  done
  echo "ERROR: SwordFS daemon ${pid} is still alive after unmount (state=${state:-unknown})" >&2
  return 1
}

echo "=== Verifying SwordFS FUSE mount lifecycle ==="
{
  echo "TEST_DEV=${TEST_DEV}"
  echo "TEST_DIR=${TEST_DIR}"
  echo "-- mount --"
  mount -t fuse.swordfs -o allow_other "${TEST_DEV}" "${TEST_DIR}"
  test_pid="$(cat "${OUTPUT_DIR}/swordfs-test.pid")"
  echo "daemon pid=${test_pid}"
  echo "-- source lookup --"
  findmnt -rncv -S "${TEST_DEV}" -o SOURCE,TARGET,FSTYPE,OPTIONS || true
  echo "-- target lookup --"
  findmnt -rncv -T "${TEST_DIR}" -o SOURCE,TARGET,FSTYPE,OPTIONS || true
  echo "-- mountinfo --"
  grep -F " ${TEST_DIR} " /proc/self/mountinfo || true

  actual_target="$(findmnt -rncv -S "${TEST_DEV}" -o TARGET | head -1)"
  if [[ "${actual_target}" != "${TEST_DIR}" ]]; then
    echo "ERROR: TEST_DEV source lookup resolved to '${actual_target:-<none>}', expected '${TEST_DIR}'"
    exit 2
  fi

  echo "-- source-based unmount --"
  umount "${TEST_DEV}"
  if mountpoint -q "${TEST_DIR}"; then
    echo "ERROR: TEST_DIR is still a mountpoint immediately after 'umount ${TEST_DEV}'"
    findmnt -rncv -T "${TEST_DIR}" -o SOURCE,TARGET,FSTYPE,OPTIONS || true
    exit 2
  fi
  wait_for_daemon_exit "${test_pid}"

  echo "-- immediate remount --"
  mount -t fuse.swordfs -o allow_other "${TEST_DEV}" "${TEST_DIR}"
  test_pid="$(cat "${OUTPUT_DIR}/swordfs-test.pid")"
  echo "remount daemon pid=${test_pid}"
  findmnt -rncv -S "${TEST_DEV}" -o SOURCE,TARGET,FSTYPE,OPTIONS || true
  umount "${TEST_DEV}"
  if mountpoint -q "${TEST_DIR}"; then
    echo "ERROR: TEST_DIR remained mounted after the second source-based unmount"
    exit 2
  fi
  wait_for_daemon_exit "${test_pid}"
  echo "mount lifecycle preflight: PASS"
} >"${RAW_DIR}/mount-lifecycle-preflight.log" 2>&1

cat >"${FSTESTS_DIR}/local.config" <<EOF
export TEST_DEV=${TEST_DEV}
export TEST_DIR=${TEST_DIR}
export SCRATCH_DEV=${SCRATCH_DEV}
export SCRATCH_MNT=${SCRATCH_MNT}
export FSTYP=fuse
export FUSE_SUBTYP=.swordfs
export MOUNT_OPTIONS="-o allow_other"
export TEST_FS_MOUNT_OPTS="-o allow_other"
EOF
cp "${FSTESTS_DIR}/local.config" "${OUTPUT_DIR}/local.config"

if [[ "${VERIFY_SELECTION}" -eq 1 ]]; then
  echo "=== Verifying pinned upstream selection ${FSTESTS_GROUP} ==="
  mkdir -p "${SELECT_RESULTS}"
  (
    cd "${FSTESTS_DIR}"
    RESULT_BASE="${SELECT_RESULTS}" ./check -n -R xunit-quiet -fuse -g "${FSTESTS_GROUP}"
  ) >"${RAW_DIR}/selection-verification.log" 2>&1
  if [[ ! -s "${SELECT_RESULTS}/result.xml" ]]; then
    echo "ERROR: fstests selection verification did not produce XUnit" >&2
    exit 2
  fi
  python3 "${PLAN_SCRIPT}" --verify-selection-xml "${SELECT_RESULTS}/result.xml"
fi

echo "=== Running fstests shard ${SHARD_NAME} with per-test isolation ==="
mkdir -p "${RUN_RESULTS}" "${XUNIT_PARTS}"

mark_isolation_failure() {
  local test="$1"
  local message="$2"
  local group="${test%%/*}"
  local id="${test##*/}"
  mkdir -p "${RUN_RESULTS}/${group}"
  printf '%s\n' "${message}" >>"${RUN_RESULTS}/${group}/${id}.isolationfail"
}

wait_pidfile() {
  local test="$1"
  local pidfile="$2"
  [[ -s "${pidfile}" ]] || return 0
  local pid
  pid="$(cat "${pidfile}" 2>/dev/null || true)"
  [[ "${pid}" =~ ^[0-9]+$ ]] || return 0
  if ! wait_for_daemon_exit "${pid}"; then
    mark_isolation_failure "${test}" "SwordFS daemon ${pid} did not exit after upstream testcase cleanup"
    kill -TERM "${pid}" 2>/dev/null || true
    sleep 0.2
    kill -KILL "${pid}" 2>/dev/null || true
    if ! wait_for_daemon_exit "${pid}"; then
      mark_isolation_failure "${test}" "SwordFS daemon ${pid} survived forced testcase isolation"
      return 1
    fi
  fi
  rm -f "${pidfile}"
}

isolate_after_test() {
  local test="$1"
  local mountpoint
  local intervened=0
  for mountpoint in "${TEST_DIR}" "${SCRATCH_MNT}"; do
    if findmnt -rn -M "${mountpoint}" -o TARGET >/dev/null 2>&1; then
      intervened=1
      bounded_fusermount -u "${mountpoint}" || \
        bounded_fusermount -uz "${mountpoint}" || true
    fi
  done
  wait_pidfile "${test}" "${OUTPUT_DIR}/swordfs-test.pid" || return 1
  wait_pidfile "${test}" "${OUTPUT_DIR}/swordfs-scratch.pid" || return 1
  for mountpoint in "${TEST_DIR}" "${SCRATCH_MNT}"; do
    if findmnt -rn -M "${mountpoint}" -o TARGET >/dev/null 2>&1; then
      mark_isolation_failure "${test}" "mountpoint remained active after forced testcase isolation: ${mountpoint}"
      return 1
    fi
  done
  if [[ "${intervened}" -ne 0 ]]; then
    mark_isolation_failure "${test}" "runner had to unmount a leftover FUSE mount after upstream testcase cleanup"
  fi
  return 0
}

backend_healthy() {
  timeout 5s "${DOCKER_COMPOSE[@]}" -f "${COMPOSE_FILE}" exec -T redis redis-cli ping >/dev/null 2>&1 && \
    curl --max-time 5 -fsS "http://127.0.0.1:${MINIO_PORT}/minio/health/live" >/dev/null 2>&1
}

suite_seconds="$(parse_timeout_seconds FSTESTS_SUITE_TIMEOUT "${FSTESTS_SUITE_TIMEOUT}")"
test_seconds="$(parse_timeout_seconds FSTESTS_TEST_TIMEOUT "${FSTESTS_TEST_TIMEOUT}")"
suite_deadline=$(( $(date +%s) + suite_seconds ))
check_status=0
timeout_reason=""
: >"${RAW_DIR}/check.log"
: >"${RAW_DIR}/check-status.tsv"
: >"${RAW_DIR}/timeout-status.tsv"

while IFS= read -r test <&3; do
  [[ -n "${test}" ]] || continue
  now="$(date +%s)"
  remaining=$((suite_deadline - now))
  if ((remaining <= 0)); then
    check_status=124
    timeout_reason="suite"
    break
  fi

  test_budget="${test_seconds}"
  test_timeout_reason="per-test"
  if ((remaining <= test_seconds)); then
    test_budget="${remaining}"
    test_timeout_reason="suite"
  fi

  echo "=== ${test} ===" >>"${RAW_DIR}/check.log"
  rm -f "${RUN_RESULTS}/result.xml"
  set +e
  (
    cd "${FSTESTS_DIR}"
    RESULT_BASE="${RUN_RESULTS}" timeout --foreground --kill-after=30s "${test_budget}s" \
      ./check -R xunit-quiet -fuse "${test}" </dev/null
  ) >>"${RAW_DIR}/check.log" 2>&1
  test_status=$?
  set -e
  printf '%s\t%s\n' "${test}" "${test_status}" >>"${RAW_DIR}/check-status.tsv"

  if [[ -s "${RUN_RESULTS}/result.xml" ]]; then
    cp "${RUN_RESULTS}/result.xml" "${XUNIT_PARTS}/${test//\//-}.xml"
  fi
  if [[ "${test_status}" -eq 124 || "${test_status}" -eq 137 ]]; then
    check_status="${test_status}"
    timeout_reason="${test_timeout_reason}"
    printf '%s\t%s\t%s\t%s\n' \
      "${test}" "${test_timeout_reason}" "${test_budget}" "${test_status}" >>"${RAW_DIR}/timeout-status.tsv"
    echo "ERROR: fstests testcase ${test} exceeded ${test_timeout_reason} timeout after ${test_budget}s" | \
      tee -a "${RAW_DIR}/check.log" >&2
    isolate_after_test "${test}" || true
    break
  fi
  if [[ "${test_status}" -ne 0 ]]; then
    check_status=1
    if ! backend_healthy; then
      mark_isolation_failure "${test}" "backend health check failed after testcase failure"
      check_status=2
      isolate_after_test "${test}" || true
      break
    fi
  fi
  if ! isolate_after_test "${test}"; then
    check_status=2
    break
  fi
done 3<"${OUTPUT_DIR}/planned-tests.txt"

expected_execute_count="$(wc -l <"${OUTPUT_DIR}/planned-tests.txt")"
completed_execute_count="$(wc -l <"${RAW_DIR}/check-status.tsv")"
if [[ "${check_status}" -ne 2 && "${check_status}" -ne 124 && "${check_status}" -ne 137 && \
      "${completed_execute_count}" -ne "${expected_execute_count}" ]]; then
  echo "ERROR: fstests runner executed ${completed_execute_count}/${expected_execute_count} planned testcases" >&2
  check_status=2
fi

python3 "${XUNIT_MERGER}" --parts-dir "${XUNIT_PARTS}" --output "${RUN_RESULTS}/result.xml"
printf '%s\n' "${check_status}" >"${RAW_DIR}/check.exit"

if [[ "${check_status}" -eq 124 || "${check_status}" -eq 137 ]]; then
  if [[ "${timeout_reason}" == "per-test" ]]; then
    echo "ERROR: fstests testcase exceeded per-test timeout ${FSTESTS_TEST_TIMEOUT}" >&2
  else
    echo "ERROR: fstests shard exceeded suite timeout ${FSTESTS_SUITE_TIMEOUT}" >&2
  fi
  exit 2
fi
if [[ "${check_status}" -eq 2 ]]; then
  echo "ERROR: fstests testcase isolation could not restore a trustworthy environment" >&2
  exit 2
fi
if [[ ! -s "${RUN_RESULTS}/result.xml" ]]; then
  echo "ERROR: fstests shard did not produce XUnit results (status ${check_status})" >&2
  exit 2
fi

# A normal fstests run returns non-zero when one or more testcases fail. The
# aggregate classifier, not the raw shard exit code, decides whether those
# failures are admitted known gaps or blocking regressions.
echo "=== fstests shard ${SHARD_NAME} raw execution complete (check status ${check_status}) ==="
