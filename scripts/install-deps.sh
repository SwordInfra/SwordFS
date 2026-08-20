#!/bin/sh
# Install all third-party dependencies needed to build SwordFS.
#
# Usage:
#   ./scripts/install-deps.sh [--force] [--skip-heavy] [--only-heavy]
#
#   --force        Rebuild and reinstall even if already present.
#   --skip-heavy   Only install system packages; skip folly + AWS SDK.
#   --only-heavy   Only build folly + AWS SDK; skip system packages.
#
# This script:
#   1. Installs system packages (libfuse3, folly build deps)
#   2. Builds and installs folly from GitHub release tarball
#   3. Installs AWS SDK for S3 object storage
#
# Each step is skipped if the dependency is already present and
# passes verification (cmake configs AND library binaries exist).
# After running this script, you can build with:
#   cmake --preset default

set -e

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
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
DEPS_PREFIX="${DEPS_PREFIX:-/usr/local}"
# shellcheck source=native-deps-cache.env
. "$SCRIPT_DIR/native-deps-cache.env"
FOLLY_SRC="$PROJECT_DIR/build/folly-src"

echo "==> Checking system packages..."

if [ "$ONLY_HEAVY" = true ]; then
  echo "==> --only-heavy: skipping system packages."
else

SYSTEM_PKGS="libfuse3-dev fuse3 libfmt-dev libboost-all-dev libssl-dev libevent-dev libdouble-conversion-dev libgoogle-glog-dev libgtest-dev libcli11-dev libcurl4-openssl-dev zlib1g-dev curl g++ cmake ninja-build git"

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
  apt-get update -qq
  # shellcheck disable=SC2086
  apt-get install -y -qq $TO_INSTALL
else
  echo "==> All system packages already installed."
fi

# Some packages on Ubuntu 24.04 are too old for our dependencies.
# Add ubuntu resolute (25.04) via a dedicated .list file so we can
# pin specific packages with -t resolute.  Using a new file avoids
# mutating the system's existing sources.list.
echo "==> Adding resolute source for newer packages..."
if [ -f /etc/apt/sources.list.d/resolute.list ]; then
  echo "  [ok] resolute source already configured"
else
  echo "deb http://archive.ubuntu.com/ubuntu resolute main universe" \
    > /etc/apt/sources.list.d/resolute.list
  apt-get update -qq
fi

# fast_float: folly v2026.07.20.00 requires fast_float >= 7.0.0
# (needs chars_format::allow_leading_plus). Ubuntu 24.04 ships 6.1.0.
echo "==> Checking fast_float version..."
if ! dpkg-query -W -f='${Version}' libfast-float-dev 2>/dev/null | grep -qE '^([89]|[1-9][0-9])\.'; then
  echo "  ==> Installing libfast-float-dev from resolute..."
  apt-get install -y -qq -t resolute libfast-float-dev
else
  echo "  [ok] libfast-float-dev >= 8.0.0"
fi

# libfuse3-dev: SwordFS README requires >= 3.18 (for no_interrupt, tmpfile).
# Ubuntu 24.04 ships 3.14.1 which is too old.
echo "==> Checking libfuse3-dev version..."
if ! dpkg-query -W -f='${Version}' libfuse3-dev 2>/dev/null | grep -qE '^3\.(1[89]|[2-9])'; then
  echo "  ==> Installing libfuse3-dev from resolute..."
  apt-get install -y -qq -t resolute libfuse3-dev
else
  echo "  [ok] libfuse3-dev >= 3.18"
fi

# binutils: GCC >= 15 emits .base64 string encoding which requires
# binutils >= 2.43.  Ubuntu 24.04 ships binutils 2.42 which is too old.
# Upgrading from resolute (25.04) ensures ABI compatibility with GCC 15.
echo "==> Checking binutils version..."
if ! as --version 2>/dev/null | grep -qE '2\.(4[3-9]|[5-9][0-9])'; then
  echo "  ==> Installing binutils from resolute..."
  apt-get install -y -qq -t resolute binutils
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

fi  # --skip-heavy

# ────────────────────────────────────────────────────────────────
# AWS SDK for C++
# ────────────────────────────────────────────────────────────────

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

fi  # --skip-heavy

echo "==> Done. Dependencies installed under: $DEPS_PREFIX"
echo "==> You can now build SwordFS with: cmake --preset default"
