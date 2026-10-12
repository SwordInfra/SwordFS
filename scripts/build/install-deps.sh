#!/bin/sh
# Install all third-party dependencies needed to build SwordFS.
#
# Usage:
#   ./scripts/build/install-deps.sh [--force] [--skip-heavy] [--only-heavy]
#
#   --force        Rebuild and reinstall even if already present.
#   --skip-heavy   Only install system packages; skip heavy dependencies.
#   --only-heavy   Only build heavy dependencies; skip system packages.
#
# This script:
#   1. Installs system packages (libfuse3, folly build deps)
#   2. Builds and installs folly from GitHub release tarball
#   3. Installs AWS SDK for S3 object storage
#   4. Builds and installs hiredis and redis-plus-plus
#
# Each step is skipped if the dependency is already present and
# passes verification (cmake configs AND library binaries exist).
# After running this script, you can build with:
#   cmake --preset default

set -e

# Phase boundaries make slow APT mirrors distinguishable from a native cache
# miss, even when the GitHub Actions step itself appears as one long install.
DEPS_PHASE=initialization
DEPS_PHASE_STARTED=$(date +%s)
deps_phase() {
  now=$(date +%s)
  echo "ci-deps phase=${DEPS_PHASE} seconds=$((now - DEPS_PHASE_STARTED))"
  DEPS_PHASE=$1
  DEPS_PHASE_STARTED=$now
  echo "ci-deps begin=${DEPS_PHASE}"
}
trap 'status=$?; now=$(date +%s); echo "ci-deps phase=${DEPS_PHASE} seconds=$((now - DEPS_PHASE_STARTED)) exit_status=${status}"' 0

# A cold runner once spent >17 minutes between the system-package cache miss
# and the first APT package output (run 38227069396). Bound that opaque
# network/provisioning tail in CI; a timeout fails visibly, never retries or
# silently uses an incomplete dependency set.
deps_apt() {
  if [ -n "${SWORDFS_CI_APT_TIMEOUT_SECONDS:-}" ]; then
    timeout --signal=TERM --kill-after=10s "${SWORDFS_CI_APT_TIMEOUT_SECONDS}" \
      apt-get -o Acquire::http::Timeout=45 -o Acquire::https::Timeout=45 "$@"
  else
    apt-get "$@"
  fi
}

FORCE=false
SKIP_HEAVY=false
ONLY_HEAVY=false
for arg in "$@"; do
  case "$arg" in
    --force)      FORCE=true ;;
    --skip-heavy) SKIP_HEAVY=true ;;
    --only-heavy) ONLY_HEAVY=true ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEPS_PREFIX="${DEPS_PREFIX:-/usr/local}"
# shellcheck source=native-deps-cache.env
. "$SCRIPT_DIR/native-deps-cache.env"
FOLLY_SRC="$PROJECT_DIR/build/folly-src"

deps_phase system-package-inventory
echo "==> Checking system packages..."

if [ "$ONLY_HEAVY" = true ]; then
  echo "==> --only-heavy: skipping system packages."
else

# Pinned Folly exports exactly context/filesystem/program_options/regex/thread
# in folly-config.cmake.  Installing libboost-all-dev also pulls unrelated
# MPI, Python, graph and RDMA development packages onto every clean CI runner.
# Keep the required Folly components (and their transitive Boost headers).
SYSTEM_PKGS="libfuse3-dev fuse3 libfmt-dev libboost-context-dev libboost-filesystem-dev libboost-program-options-dev libboost-regex-dev libboost-thread-dev libssl-dev libevent-dev libdouble-conversion-dev libgoogle-glog-dev libgtest-dev libcli11-dev libcurl4-openssl-dev zlib1g-dev curl g++ cmake ninja-build git"

TO_INSTALL=""
for pkg in $SYSTEM_PKGS; do
  if dpkg-query -W -f='${Status}' "$pkg" 2>/dev/null | grep -q 'install ok installed'; then
    echo "  [ok] $pkg"
  else
    echo "  [missing] $pkg"
    TO_INSTALL="$TO_INSTALL $pkg"
  fi
done

if [ -n "$TO_INSTALL" ]; then
  echo "==> Installing missing system packages:$TO_INSTALL"
  deps_phase system-apt-index
  deps_apt update -qq
  deps_phase system-apt-install
  # shellcheck disable=SC2086
  deps_apt install -y -qq $TO_INSTALL
else
  echo "==> All system packages already installed."
fi

# Some packages on Ubuntu 24.04 are too old for our dependencies.
# Add ubuntu resolute (25.04) via a dedicated .list file so we can
# pin specific packages with -t resolute.  Using a new file avoids
# mutating the system's existing sources.list.
deps_phase resolute-source-and-index
echo "==> Adding resolute source for newer packages..."
if [ -f /etc/apt/sources.list.d/resolute.list ]; then
  echo "  [ok] resolute source already configured"
else
  echo "deb http://archive.ubuntu.com/ubuntu resolute main universe" \
    > /etc/apt/sources.list.d/resolute.list
  deps_apt update -qq
fi

# fast_float: folly v2026.07.20.00 requires fast_float >= 7.0.0
# (needs chars_format::allow_leading_plus). Ubuntu 24.04 ships 6.1.0.
deps_phase resolute-fast-float
echo "==> Checking fast_float version..."
if ! dpkg-query -W -f='${Version}' libfast-float-dev 2>/dev/null | grep -qE '^([89]|[1-9][0-9])\.'; then
  echo "  ==> Installing libfast-float-dev from resolute..."
  deps_apt install -y -qq -t resolute libfast-float-dev
else
  echo "  [ok] libfast-float-dev >= 8.0.0"
fi

# libfuse3-dev: SwordFS README requires >= 3.18 (for no_interrupt, tmpfile).
# Ubuntu 24.04 ships 3.14.1 which is too old.
deps_phase resolute-fuse
echo "==> Checking libfuse3-dev version..."
if ! dpkg-query -W -f='${Version}' libfuse3-dev 2>/dev/null | grep -qE '^3\.(1[89]|[2-9])'; then
  echo "  ==> Installing libfuse3-dev from resolute..."
  deps_apt install -y -qq -t resolute libfuse3-dev
else
  echo "  [ok] libfuse3-dev >= 3.18"
fi

# binutils: GCC >= 15 emits .base64 string encoding which requires
# binutils >= 2.43.  Ubuntu 24.04 ships binutils 2.42 which is too old.
# Upgrading from resolute (25.04) ensures ABI compatibility with GCC 15.
deps_phase resolute-binutils
echo "==> Checking binutils version..."
if ! as --version 2>/dev/null | grep -qE '2\.(4[3-9]|[5-9][0-9])'; then
  echo "  ==> Installing binutils from resolute..."
  deps_apt install -y -qq -t resolute binutils
else
  echo "  [ok] binutils >= 2.43"
fi

fi  # --only-heavy

# ────────────────────────────────────────────────────────────────
# Helper: verify that an installed dependency is actually usable.
# Returns 0 (success) if find_package works, non-zero otherwise.
# ────────────────────────────────────────────────────────────────

verify_folly() {
  TMPDIR=$(mktemp -d)
  cat > "$TMPDIR/CMakeLists.txt" << 'CMEOF'
cmake_minimum_required(VERSION 3.16)
project(VerifyFolly CXX)
find_package(folly REQUIRED)
message(STATUS "folly OK")
CMEOF
  cd "$TMPDIR"
  if cmake -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" . > /dev/null 2>&1; then
    rm -rf "$TMPDIR"
    return 0
  fi
  rm -rf "$TMPDIR"
  return 1
}

verify_redis_plus_plus() {
  TMPDIR=$(mktemp -d)
  cat > "$TMPDIR/CMakeLists.txt" << 'CMEOF'
cmake_minimum_required(VERSION 3.16)
project(VerifyRedisPlusPlus CXX)
find_package(redis++ CONFIG REQUIRED)
if(NOT TARGET redis++::redis++_static)
  message(FATAL_ERROR "redis++::redis++_static target not found")
endif()
message(STATUS "redis-plus-plus OK")
CMEOF
  cd "$TMPDIR"
  if cmake -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" . > /dev/null 2>&1; then
    rm -rf "$TMPDIR"
    return 0
  fi
  rm -rf "$TMPDIR"
  return 1
}

verify_aws_sdk() {
  TMPDIR=$(mktemp -d)
  cat > "$TMPDIR/CMakeLists.txt" << 'CMEOF'
cmake_minimum_required(VERSION 3.16)
project(VerifyAwsSdk CXX)
find_package(AWSSDK REQUIRED COMPONENTS s3)
message(STATUS "AWSSDK OK")
CMEOF
  cd "$TMPDIR"
  if cmake -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" . > /dev/null 2>&1; then
    rm -rf "$TMPDIR"
    return 0
  fi
  rm -rf "$TMPDIR"
  return 1
}

# ────────────────────────────────────────────────────────────────
# folly
# ────────────────────────────────────────────────────────────────

deps_phase folly-cache-verification
echo "==> Checking folly..."

if [ "$SKIP_HEAVY" = true ]; then
  echo "==> --skip-heavy: assuming folly already installed, skipping."
else

FOLLY_INSTALLED=false
if [ "$FORCE" = false ]; then
  if [ -f "$DEPS_PREFIX/lib/cmake/folly/folly-config.cmake" ]; then
    # Also verify the actual library file exists (not just cmake config).
    if [ -f "$DEPS_PREFIX/lib/libfolly.a" ] || [ -f "$DEPS_PREFIX/lib/libfolly.so" ]; then
      echo "==> folly cmake config and library found, verifying..."
      if verify_folly; then
        echo "==> folly verified OK, skipping."
        FOLLY_INSTALLED=true
      else
        echo "==> folly verification FAILED, will reinstall."
      fi
    else
      echo "==> folly cmake config found but library missing, will reinstall."
    fi
  fi
fi

if [ "$FOLLY_INSTALLED" = false ]; then
  deps_phase folly-download-build-install
  # ── Step 1: Download ─────────────────────────────────────────

  FOLLY_TARBALL="$PROJECT_DIR/build/folly-${FOLLY_VER}.tar.gz"
  mkdir -p "$PROJECT_DIR/build"
  if [ -f "$FOLLY_TARBALL" ]; then
    echo "==> folly tarball already downloaded, skipping."
  else
    echo "==> Downloading folly ${FOLLY_VER}..."
    FOLLY_URL="https://github.com/facebook/folly/archive/refs/tags/${FOLLY_VER}.tar.gz"
    curl -sL "$FOLLY_URL" -o "$FOLLY_TARBALL"
  fi

  # ── Step 2: Extract ──────────────────────────────────────────

  if [ -f "$FOLLY_SRC/CMakeLists.txt" ]; then
    echo "==> folly already extracted, skipping."
  else
    echo "==> Extracting folly..."
    rm -rf "$FOLLY_SRC"
    mkdir -p "$FOLLY_SRC"
    tar xzf "$FOLLY_TARBALL" -C "$FOLLY_SRC" --strip-components=1
  fi

  # ── Step 3: Configure (skip if already done) ─────────────────

  if [ -f "$FOLLY_SRC/build/CMakeCache.txt" ]; then
    echo "==> folly already configured, skipping."
  else
    echo "==> Configuring folly..."
    mkdir -p "$FOLLY_SRC/build"
    cd "$FOLLY_SRC/build"
    cmake .. \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DBoost_NO_BOOST_CMAKE=ON \
      -DBoost_SYSTEM_FOUND=ON
  fi

  # ── Step 4: Build & Install ──────────────────────────────────

  echo "==> Building and installing folly..."
  cd "$FOLLY_SRC/build"
  cmake --build . -j"$(nproc)"
  cmake --install .
fi

echo "ci-deps cache=folly hit=${FOLLY_INSTALLED}"

fi  # --skip-heavy

# ────────────────────────────────────────────────────────────────
# AWS SDK for C++
# ────────────────────────────────────────────────────────────────

deps_phase aws-sdk-cache-verification
echo "==> Checking AWS SDK for C++..."

AWS_SDK_SRC="$PROJECT_DIR/build/aws-sdk-src"

if [ "$SKIP_HEAVY" = true ]; then
  echo "==> --skip-heavy: assuming AWS SDK already installed, skipping."
else

AWS_SDK_INSTALLED=false
if [ "$FORCE" = false ]; then
  if [ -f "$DEPS_PREFIX/lib/cmake/aws-cpp-sdk-s3/aws-cpp-sdk-s3-config.cmake" ]; then
    # Also verify the actual core library exists (not just cmake config).
    if [ -f "$DEPS_PREFIX/lib/libaws-cpp-sdk-core.a" ] || \
       [ -f "$DEPS_PREFIX/lib/libaws-cpp-sdk-core.so" ]; then
      echo "==> AWS SDK cmake config and library found, verifying..."
      if verify_aws_sdk; then
        echo "==> AWS SDK verified OK, skipping."
        AWS_SDK_INSTALLED=true
      else
        echo "==> AWS SDK verification FAILED, will reinstall."
      fi
    else
      echo "==> AWS SDK cmake config found but library missing, will reinstall."
    fi
  fi
fi

if [ "$AWS_SDK_INSTALLED" = false ]; then
  deps_phase aws-sdk-download-build-install
  # ── Step 1: Clone with submodules ──────────────────────────

  if [ -f "$AWS_SDK_SRC/CMakeLists.txt" ]; then
    echo "==> AWS SDK already cloned, skipping."
  else
    echo "==> Cloning AWS SDK ${AWS_SDK_VER} (with submodules)..."
    git clone --recurse-submodules \
      --depth 1 --branch "${AWS_SDK_VER}" \
      https://github.com/aws/aws-sdk-cpp.git "$AWS_SDK_SRC"
  fi

  # ── Step 2: Configure ──────────────────────────────────────

  if [ -f "$AWS_SDK_SRC/build/CMakeCache.txt" ]; then
    echo "==> AWS SDK already configured, skipping."
  else
    echo "==> Configuring AWS SDK..."
    mkdir -p "$AWS_SDK_SRC/build"
    cd "$AWS_SDK_SRC/build"
    cmake .. \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
      -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ \
      -DBUILD_ONLY="$AWS_SDK_BUILD_ONLY" \
      -DBUILD_SHARED_LIBS="$AWS_SDK_BUILD_SHARED_LIBS" \
      -DOPENSSL_USE_STATIC_LIBS=OFF \
      -DENABLE_TESTING=OFF \
      -DAUTORUN_UNIT_TESTS=OFF \
      -DFORCE_SHARED_CRT=ON
  fi

  # ── Step 3: Build & Install ────────────────────────────────

  echo "==> Building and installing AWS SDK..."
  cd "$AWS_SDK_SRC/build"
  cmake --build . -j"$(nproc)"
  cmake --install .
fi

echo "ci-deps cache=aws-sdk hit=${AWS_SDK_INSTALLED}"

fi  # --skip-heavy

# ────────────────────────────────────────────────────────────────
# hiredis + redis-plus-plus
# ────────────────────────────────────────────────────────────────

deps_phase redis-plus-plus-cache-verification
echo "==> Checking redis-plus-plus..."

REDIS_PLUS_PLUS_INSTALLED=false
if [ "$FORCE" = false ]; then
  if [ -f "$DEPS_PREFIX/share/cmake/redis++/redis++-config.cmake" ] && \
     { [ -f "$DEPS_PREFIX/lib/libredis++_static.a" ] || [ -f "$DEPS_PREFIX/lib/libredis++.a" ]; }; then
    echo "==> redis-plus-plus cmake config and library found, verifying..."
    if verify_redis_plus_plus; then
      echo "==> redis-plus-plus verified OK, skipping."
      REDIS_PLUS_PLUS_INSTALLED=true
    else
      echo "==> redis-plus-plus verification FAILED, will reinstall."
    fi
  fi
fi

if [ "$REDIS_PLUS_PLUS_INSTALLED" = false ]; then
  deps_phase redis-plus-plus-download-build-install
  HIREDIS_SRC="$PROJECT_DIR/build/hiredis-src"
  REDIS_PLUS_PLUS_SRC="$PROJECT_DIR/build/redis-plus-plus-src"

  if [ -f "$DEPS_PREFIX/lib/cmake/hiredis/hiredis-config.cmake" ]; then
    echo "==> hiredis already installed, skipping."
  else
    if [ -f "$HIREDIS_SRC/CMakeLists.txt" ]; then
      echo "==> hiredis already cloned, skipping."
    else
      echo "==> Cloning hiredis ${HIREDIS_VER}..."
      git clone --depth 1 --branch "v${HIREDIS_VER}" \
        https://github.com/redis/hiredis.git "$HIREDIS_SRC"
    fi
    if [ -f "$HIREDIS_SRC/build/CMakeCache.txt" ]; then
      echo "==> hiredis already configured, skipping."
    else
      echo "==> Configuring hiredis..."
      mkdir -p "$HIREDIS_SRC/build"
      cd "$HIREDIS_SRC/build"
      cmake .. \
        -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
        -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
        -DBUILD_SHARED_LIBS="$REDIS_PLUS_PLUS_BUILD_SHARED_LIBS" \
        -DDISABLE_TESTS=ON
    fi
    echo "==> Building and installing hiredis..."
    cd "$HIREDIS_SRC/build"
    cmake --build . -j"$(nproc)"
    cmake --install .
  fi

  if [ -f "$REDIS_PLUS_PLUS_SRC/CMakeLists.txt" ]; then
    echo "==> redis-plus-plus already cloned, skipping."
  else
    echo "==> Cloning redis-plus-plus ${REDIS_PLUS_PLUS_VER}..."
    git clone --depth 1 --branch "${REDIS_PLUS_PLUS_VER}" \
      https://github.com/sewenew/redis-plus-plus.git "$REDIS_PLUS_PLUS_SRC"
  fi
  if [ -f "$REDIS_PLUS_PLUS_SRC/build/CMakeCache.txt" ]; then
    echo "==> redis-plus-plus already configured, skipping."
  else
    echo "==> Configuring redis-plus-plus..."
    mkdir -p "$REDIS_PLUS_PLUS_SRC/build"
    cd "$REDIS_PLUS_PLUS_SRC/build"
    cmake .. \
      -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
      -DCMAKE_INSTALL_PREFIX="$DEPS_PREFIX" \
      -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" \
      -DREDIS_PLUS_PLUS_BUILD_STATIC=ON \
      -DREDIS_PLUS_PLUS_BUILD_SHARED=OFF \
      -DREDIS_PLUS_PLUS_BUILD_TEST=OFF
  fi
  echo "==> Building and installing redis-plus-plus..."
  cd "$REDIS_PLUS_PLUS_SRC/build"
  cmake --build . -j"$(nproc)"
  cmake --install .
fi

echo "ci-deps cache=redis-plus-plus hit=${REDIS_PLUS_PLUS_INSTALLED}"

deps_phase complete
echo "==> Done. Dependencies installed under: $DEPS_PREFIX"
echo "==> You can now build SwordFS with: cmake --preset default"
