#!/bin/sh
# Install all dependencies needed to build SwordFS.
#
# Usage:
#   sudo ./scripts/install-deps.sh [--force] [--skip-heavy]
#
#   --force        Rebuild heavy deps (folly, AWS SDK) even if already present.
#   --skip-heavy   Only install system packages & resolute upgrades;
#                  skip folly and AWS SDK (useful when cached by CI).

set -eu

FORCE=false
SKIP_HEAVY=false
for arg in "$@"; do
  case "$arg" in
    --force) FORCE=true ;;
    --skip-heavy) SKIP_HEAVY=true ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
NPROC="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

# ═══════════════════════════════════════════════════════════════
# System packages
# ═══════════════════════════════════════════════════════════════

echo "==> Checking system packages..."

# Style note: variable on its own line to avoid overlong lines.
SYSTEM_PKGS="libfuse3-dev fuse3 libfmt-dev libboost-all-dev libssl-dev
  libevent-dev libdouble-conversion-dev libgoogle-glog-dev libgtest-dev
  libcli11-dev libcurl4-openssl-dev zlib1g-dev curl g++ cmake ninja-build git"

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

# ═══════════════════════════════════════════════════════════════
# Resolute (Ubuntu 25.04) backports for Ubuntu 24.04
# ═══════════════════════════════════════════════════════════════

echo "==> Adding resolute source for newer packages..."
if [ -f /etc/apt/sources.list.d/resolute.list ]; then
  echo "  [ok] resolute source already configured"
else
  echo "deb http://archive.ubuntu.com/ubuntu resolute main universe" \
    > /etc/apt/sources.list.d/resolute.list
  apt-get update -qq
fi

# fast_float: folly v2026.07.20.00 requires >= 7.0.0.
# Ubuntu 24.04 ships 6.1.0.
echo "==> Checking fast_float version..."
if ! dpkg-query -W -f='${Version}' libfast-float-dev 2>/dev/null | grep -qE '^([89]|[1-9][0-9])\.'; then
  echo "  ==> Installing libfast-float-dev from resolute..."
  apt-get install -y -qq -t resolute libfast-float-dev
else
  echo "  [ok] libfast-float-dev >= 8.0.0"
fi

# libfuse3-dev: requires >= 3.18 (for no_interrupt, tmpfile).
# Ubuntu 24.04 ships 3.14.1.
echo "==> Checking libfuse3-dev version..."
if ! dpkg-query -W -f='${Version}' libfuse3-dev 2>/dev/null | grep -qE '^3\.(1[89]|[2-9])'; then
  echo "  ==> Installing libfuse3-dev from resolute..."
  apt-get install -y -qq -t resolute libfuse3-dev
else
  echo "  [ok] libfuse3-dev >= 3.18"
fi

# binutils: GCC >= 15 emits .base64 strings requiring binutils >= 2.43.
# Ubuntu 24.04 ships 2.42.
echo "==> Checking binutils version..."
if ! as --version 2>/dev/null | grep -qE '2\.(4[3-9]|[5-9][0-9])'; then
  echo "  ==> Installing binutils from resolute..."
  apt-get install -y -qq -t resolute binutils
else
  echo "  [ok] binutils >= 2.43"
fi

# ═══════════════════════════════════════════════════════════════
# Heavy dependencies (folly + AWS SDK) — skip with --skip-heavy
# ═══════════════════════════════════════════════════════════════

if [ "$SKIP_HEAVY" = true ]; then
  echo "==> --skip-heavy: done (system packages only)."
  exit 0
fi

# ── folly ──────────────────────────────────────────────────────

FOLLY_VER="v2026.07.20.00"
FOLLY_SRC="$PROJECT_DIR/build/folly-src"
FOLLY_BUILD="$FOLLY_SRC/build"

NEED_FOLLY=true
if [ "$FORCE" = false ] && \
   [ -f /usr/local/lib/cmake/folly/folly-config.cmake ] && \
   { [ -f /usr/local/lib/libfolly.a ] || [ -f /usr/local/lib/libfolly.so ]; }; then
  echo "==> folly already installed, use --force to rebuild."
  NEED_FOLLY=false
fi

if [ "$NEED_FOLLY" = true ]; then
  echo "==> Installing folly ${FOLLY_VER}..."

  FOLLY_TARBALL="$PROJECT_DIR/build/folly-${FOLLY_VER}.tar.gz"
  mkdir -p "$PROJECT_DIR/build"
  if [ ! -f "$FOLLY_TARBALL" ]; then
    echo "  -> Downloading..."
    curl -sL "https://github.com/facebook/folly/archive/refs/tags/${FOLLY_VER}.tar.gz" \
      -o "$FOLLY_TARBALL"
  fi

  if [ ! -f "$FOLLY_SRC/CMakeLists.txt" ]; then
    echo "  -> Extracting..."
    rm -rf "$FOLLY_SRC"
    mkdir -p "$FOLLY_SRC"
    tar xzf "$FOLLY_TARBALL" -C "$FOLLY_SRC" --strip-components=1
  fi

  echo "  -> Configuring..."
  mkdir -p "$FOLLY_BUILD"
  cd "$FOLLY_BUILD"
  cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DBoost_NO_BOOST_CMAKE=ON \
    -DBoost_SYSTEM_FOUND=ON

  echo "  -> Building (${NPROC} jobs)..."
  cmake --build . -j"$NPROC"
  cmake --install .
  echo "  [ok] folly installed"
fi

# ── AWS SDK ────────────────────────────────────────────────────

AWS_SDK_VER="1.11.540"
AWS_SDK_SRC="$PROJECT_DIR/build/aws-sdk-src"
AWS_SDK_BUILD="$AWS_SDK_SRC/build"

NEED_AWS=true
if [ "$FORCE" = false ] && \
   [ -f /usr/local/lib/cmake/aws-cpp-sdk-s3/aws-cpp-sdk-s3-config.cmake ] && \
   { [ -f /usr/local/lib/libaws-cpp-sdk-core.a ] || \
     [ -f /usr/local/lib/libaws-cpp-sdk-core.so ]; }; then
  echo "==> AWS SDK already installed, use --force to rebuild."
  NEED_AWS=false
fi

if [ "$NEED_AWS" = true ]; then
  echo "==> Installing AWS SDK ${AWS_SDK_VER}..."

  if [ ! -f "$AWS_SDK_SRC/CMakeLists.txt" ]; then
    echo "  -> Cloning (with submodules)..."
    git clone --recurse-submodules \
      --depth 1 --branch "${AWS_SDK_VER}" \
      https://github.com/aws/aws-sdk-cpp.git "$AWS_SDK_SRC"
  fi

  echo "  -> Configuring..."
  mkdir -p "$AWS_SDK_BUILD"
  cd "$AWS_SDK_BUILD"
  cmake .. \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -DBUILD_ONLY="s3" \
    -DBUILD_SHARED_LIBS=OFF \
    -DOPENSSL_USE_STATIC_LIBS=OFF \
    -DENABLE_TESTING=OFF \
    -DAUTORUN_UNIT_TESTS=OFF \
    -DFORCE_SHARED_CRT=ON

  echo "  -> Building (${NPROC} jobs)..."
  cmake --build . -j"$NPROC"
  cmake --install .
  echo "  [ok] AWS SDK installed"
fi

echo "==> Done. You can now build SwordFS with: cmake --preset default"
