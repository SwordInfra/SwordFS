#!/bin/sh
# Install all third-party dependencies needed to build SwordFS.
#
# Usage:
#   ./scripts/install-deps.sh [--with-s3]
#
#   --with-s3   Also install AWS SDK for C++ (libaws-sdk-s3-dev)
#
# This script:
#   1. Installs system packages (libfuse3, folly build deps)
#   2. Builds and installs folly from GitHub release tarball
#   3. (optional) Installs AWS SDK for S3 object storage
#
# Each step is skipped if the dependency is already present.
# After running this script, you can configure with:
#   cmake --preset default          # without S3
#   cmake --preset default -DENABLE_S3=ON    # with S3

set -e

WITH_S3=false
for arg in "$@"; do
  case "$arg" in
    --with-s3) WITH_S3=true ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
FOLLY_SRC="$PROJECT_DIR/build/folly-src"
FOLLY_VER="v2026.07.20.00"

echo "==> Checking system packages..."

SYSTEM_PKGS="libfuse3-dev fuse3 libfmt-dev libboost-all-dev libssl-dev libevent-dev libdouble-conversion-dev libgoogle-glog-dev libgtest-dev libcli11-dev curl g++ cmake ninja-build git"

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
echo "deb http://archive.ubuntu.com/ubuntu resolute main universe" \
  > /etc/apt/sources.list.d/resolute.list
apt-get update -qq

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

# ────────────────────────────────────────────────────────────────
# folly
# ────────────────────────────────────────────────────────────────

echo "==> Checking folly..."

if [ -f /usr/local/lib/cmake/folly/folly-config.cmake ] || \
   [ -f /usr/lib/cmake/folly/folly-config.cmake ]; then
  echo "==> folly already installed, skipping."
else
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
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local \
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

# ────────────────────────────────────────────────────────────────
# AWS SDK for C++ (optional — only when --with-s3)
# ────────────────────────────────────────────────────────────────

if [ "$WITH_S3" = true ]; then
  echo "==> Checking AWS SDK for C++..."

  AWS_SDK_VER="1.11.540"
  AWS_SDK_SRC="$PROJECT_DIR/build/aws-sdk-src"

  if [ -f /usr/local/lib/cmake/aws-cpp-sdk-s3/aws-cpp-sdk-s3-config.cmake ]; then
    echo "==> AWS SDK already installed, skipping."
  else
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
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DBUILD_ONLY="s3" \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_TESTING=OFF \
        -DAUTORUN_UNIT_TESTS=OFF
    fi

    # ── Step 3: Build & Install ────────────────────────────────

    echo "==> Building and installing AWS SDK..."
    cd "$AWS_SDK_SRC/build"
    cmake --build . -j"$(nproc)"
    cmake --install .
  fi

  echo "==> Done. You can now build SwordFS with: cmake --preset default -DENABLE_S3=ON"
else
  echo "==> Done. You can now build SwordFS."
fi
