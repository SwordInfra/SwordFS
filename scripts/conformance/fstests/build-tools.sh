#!/usr/bin/env bash
# Build the exact upstream tools used by every fstests shard, at a stable
# absolute prefix. liburing's installed pkg-config and ELF RPATHs use this
# prefix, including when vfstest is executed in glibc secure-execution mode.
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: $0 TOOL_ROOT LOG_ROOT" >&2
  exit 2
fi
ROOT="$1"
LOG_ROOT="$2"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
source "${PROJECT_DIR}/conformance/fstests/version.env"
PATCH_RELATIVE="conformance/fstests/patches/generic-615-graceful-stat-loop-exit.patch"
PATCH="${PROJECT_DIR}/${PATCH_RELATIVE}"

if [[ "${ROOT}" != /* || -e "${ROOT}" ]]; then
  echo "ERROR: TOOL_ROOT must be a fresh absolute directory: ${ROOT}" >&2
  exit 2
fi
mkdir -p "${ROOT}" "${LOG_ROOT}"
FSTESTS_DIR="${ROOT}/xfstests"
LIBURING_DIR="${ROOT}/liburing-src"
LIBURING_PREFIX="${ROOT}/liburing-install"

echo "=== Fetching pinned liburing ${LIBURING_COMMIT} ==="
git init -q "${LIBURING_DIR}"
git -C "${LIBURING_DIR}" remote add origin "${LIBURING_REPOSITORY}"
git -C "${LIBURING_DIR}" fetch -q --depth 1 origin "${LIBURING_COMMIT}"
git -C "${LIBURING_DIR}" checkout -q --detach FETCH_HEAD
if [[ "$(git -C "${LIBURING_DIR}" rev-parse HEAD)" != "${LIBURING_COMMIT}" ]]; then
  echo "ERROR: pinned liburing checkout mismatch" >&2
  exit 2
fi

echo "=== Building pinned liburing ==="
(
  cd "${LIBURING_DIR}"
  ./configure --prefix="${LIBURING_PREFIX}"
  jobs=$(( $(nproc) / 2 ))
  ((jobs > 0)) || jobs=1
  make -j"${jobs}"
  make install
) >"${LOG_ROOT}/liburing-build.log" 2>&1

export PKG_CONFIG_PATH="${LIBURING_PREFIX}/lib/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}"
export CPPFLAGS="-I${LIBURING_PREFIX}/include${CPPFLAGS:+ ${CPPFLAGS}}"
export LDFLAGS="-L${LIBURING_PREFIX}/lib -Wl,-rpath,${LIBURING_PREFIX}/lib${LDFLAGS:+ ${LDFLAGS}}"
export LIBRARY_PATH="${LIBURING_PREFIX}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export LD_LIBRARY_PATH="${LIBURING_PREFIX}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

echo "=== Fetching pinned fstests ${FSTESTS_COMMIT} ==="
git init -q "${FSTESTS_DIR}"
git -C "${FSTESTS_DIR}" remote add origin "${FSTESTS_REPOSITORY}"
git -C "${FSTESTS_DIR}" fetch -q --depth 1 origin "${FSTESTS_COMMIT}"
git -C "${FSTESTS_DIR}" checkout -q --detach FETCH_HEAD
if [[ "$(git -C "${FSTESTS_DIR}" rev-parse HEAD)" != "${FSTESTS_COMMIT}" ]]; then
  echo "ERROR: pinned fstests checkout mismatch" >&2
  exit 2
fi

# Keep the generic/615 FUSE-pipe reliability patch unmodified.
git -C "${FSTESTS_DIR}" apply --check "${PATCH}"
git -C "${FSTESTS_DIR}" apply "${PATCH}"
git -C "${FSTESTS_DIR}" diff --check
git -C "${FSTESTS_DIR}" diff -- tests/generic/615 > "${ROOT}/fstests-local-patches.diff"

echo "=== Building pinned fstests ==="
(
  cd "${FSTESTS_DIR}"
  make configure
  ./configure
  jobs=$(( $(nproc) / 2 ))
  ((jobs > 0)) || jobs=1
  make -j"${jobs}"
) >"${LOG_ROOT}/fstests-build.log" 2>&1

# Only the installed liburing and the full working xfstests source/build tree
# are needed at test runtime, not git history nor liburing compiler objects.
rm -rf "${LIBURING_DIR}" "${FSTESTS_DIR}/.git"
test -x "${FSTESTS_DIR}/check"
test -s "${ROOT}/fstests-local-patches.diff"
test -d "${LIBURING_PREFIX}/lib"
echo "=== Shared fstests/liburing toolchain ready at ${ROOT} ==="
