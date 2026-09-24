#!/usr/bin/env bash
# Shared pinned MinIO test dependency lifecycle for E2E/conformance CI.
# Source this file; it intentionally does not enable/modify caller shell options.

_MINIO_HELPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_MINIO_PROJECT_DIR="$(cd "${_MINIO_HELPER_DIR}/../.." && pwd)"

MINIO_SERVER_VERSION="RELEASE.2025-09-07T16-13-09Z"
MINIO_MC_VERSION="RELEASE.2025-08-13T08-35-41Z"
MINIO_TOOLS_DIR="${MINIO_TOOLS_DIR:-${_MINIO_PROJECT_DIR}/.deps/minio}"
MINIO_BIN="${MINIO_BIN:-${MINIO_TOOLS_DIR}/minio}"
MC_BIN="${MC_BIN:-${MINIO_TOOLS_DIR}/mc}"

_minio_test_asset_info() {
  case "$(uname -m)" in
    x86_64|amd64)
      MINIO_ASSET_ARCH="linux-amd64"
      MINIO_SERVER_SHA256="7c5bd8512c6e966455b1d198209358b2d191c77a83ab377c4073281065fb855f"
      MINIO_MC_SHA256="01f866e9c5f9b87c2b09116fa5d7c06695b106242d829a8bb32990c00312e891"
      ;;
    aarch64|arm64)
      MINIO_ASSET_ARCH="linux-arm64"
      MINIO_SERVER_SHA256="5c83cd2cf151717ba0243f73e1c7802ff36e272b67144bdd7f1f7d684fd6f03d"
      MINIO_MC_SHA256="14c8c9616cfce4636add161304353244e8de383b2e2752c0e9dad01d4c27c12c"
      ;;
    *)
      echo "ERROR: unsupported architecture for pinned MinIO test tools: $(uname -m)" >&2
      return 2
      ;;
  esac
}

_minio_test_download_verified() {
  local url="$1"
  local expected_sha="$2"
  local destination="$3"
  local tmp="${destination}.tmp.$$"

  if [[ -f "${destination}" ]] && printf '%s  %s\n' "${expected_sha}" "${destination}" | sha256sum -c - >/dev/null 2>&1; then
    chmod +x "${destination}"
    return 0
  fi

  rm -f "${tmp}"
  curl --fail --location --retry 3 --retry-all-errors --connect-timeout 15 \
    --output "${tmp}" "${url}"
  printf '%s  %s\n' "${expected_sha}" "${tmp}" | sha256sum -c - >/dev/null
  chmod +x "${tmp}"
  mv "${tmp}" "${destination}"
}

minio_test_ensure_tools() {
  _minio_test_asset_info
  mkdir -p "${MINIO_TOOLS_DIR}"

  local server_url="https://github.com/minio/minio/releases/download/${MINIO_SERVER_VERSION}/minio.${MINIO_ASSET_ARCH}.${MINIO_SERVER_VERSION}"
  local mc_url="https://github.com/minio/mc/releases/download/${MINIO_MC_VERSION}/mc.${MINIO_ASSET_ARCH}.${MINIO_MC_VERSION}"

  _minio_test_download_verified "${server_url}" "${MINIO_SERVER_SHA256}" "${MINIO_BIN}"
  _minio_test_download_verified "${mc_url}" "${MINIO_MC_SHA256}" "${MC_BIN}"
}

minio_test_start() {
  local state_dir="$1"
  : "${MINIO_PORT:=9000}"
  : "${MINIO_ROOT_USER:=minioadmin}"
  : "${MINIO_ROOT_PASSWORD:=minioadmin}"

  minio_test_ensure_tools
  MINIO_TEST_STATE_DIR="${state_dir}"
  MINIO_TEST_PIDFILE="${state_dir}/minio.pid"
  MINIO_TEST_LOG="${state_dir}/minio.log"
  MINIO_TEST_DATA="${state_dir}/data"
  export MINIO_TEST_STATE_DIR MINIO_TEST_PIDFILE MINIO_TEST_LOG MINIO_TEST_DATA

  rm -rf "${MINIO_TEST_DATA}"
  mkdir -p "${MINIO_TEST_DATA}" "${state_dir}/mc"
  : >"${MINIO_TEST_LOG}"

  MINIO_ROOT_USER="${MINIO_ROOT_USER}" \
  MINIO_ROOT_PASSWORD="${MINIO_ROOT_PASSWORD}" \
    "${MINIO_BIN}" server "${MINIO_TEST_DATA}" --address ":${MINIO_PORT}" \
      >"${MINIO_TEST_LOG}" 2>&1 &
  local pid=$!
  printf '%s\n' "${pid}" >"${MINIO_TEST_PIDFILE}"

  local ready=0
  for _ in $(seq 1 120); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      echo "ERROR: MinIO exited before becoming ready" >&2
      cat "${MINIO_TEST_LOG}" >&2 || true
      return 2
    fi
    if curl --max-time 2 -fsS "http://127.0.0.1:${MINIO_PORT}/minio/health/live" >/dev/null 2>&1; then
      ready=1
      break
    fi
    sleep 0.25
  done
  if [[ "${ready}" -ne 1 ]]; then
    echo "ERROR: MinIO did not become ready on port ${MINIO_PORT}" >&2
    cat "${MINIO_TEST_LOG}" >&2 || true
    return 2
  fi
}

minio_test_create_bucket() {
  local bucket="$1"
  local config_dir="${MINIO_TEST_STATE_DIR:?minio_test_start must run first}/mc"
  "${MC_BIN}" --config-dir "${config_dir}" alias set local "http://127.0.0.1:${MINIO_PORT}" \
    "${MINIO_ROOT_USER}" "${MINIO_ROOT_PASSWORD}"
  "${MC_BIN}" --config-dir "${config_dir}" mb "local/${bucket}" --ignore-existing
}

minio_test_stop() {
  local pid=""
  if [[ -n "${MINIO_TEST_PIDFILE:-}" && -s "${MINIO_TEST_PIDFILE}" ]]; then
    pid="$(cat "${MINIO_TEST_PIDFILE}" 2>/dev/null || true)"
  fi
  if [[ ! "${pid}" =~ ^[0-9]+$ ]]; then
    return 0
  fi

  kill -TERM "${pid}" 2>/dev/null || true
  for _ in $(seq 1 80); do
    if ! kill -0 "${pid}" 2>/dev/null; then
      rm -f "${MINIO_TEST_PIDFILE}"
      return 0
    fi
    sleep 0.05
  done
  kill -KILL "${pid}" 2>/dev/null || true
  wait "${pid}" 2>/dev/null || true
  rm -f "${MINIO_TEST_PIDFILE}"
}

minio_test_print_versions() {
  minio_test_ensure_tools
  "${MINIO_BIN}" --version
  "${MC_BIN}" --version
}
