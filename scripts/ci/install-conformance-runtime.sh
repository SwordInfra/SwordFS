#!/usr/bin/env bash
# Install pinned conformance tools in one bounded APT transaction. The fstests
# producer needs development packages to compile its artifact; the consumers
# must not repeat that build environment on eleven clean runners.
set -euo pipefail

if [[ "$#" -ne 1 || ( "$1" != "fstests" && "$1" != "fstests-build" && "$1" != "pjdfstest" ) ]]; then
  echo "usage: $0 {fstests|fstests-build|pjdfstest}" >&2
  exit 2
fi

# Keep the same upstream package versions that produced the checked-in
# conformance baseline; Ubuntu 24.04's defaults change xfs_io probe reasons.
echo 'deb http://archive.ubuntu.com/ubuntu resolute main universe' > /etc/apt/sources.list.d/swordfs-conformance-resolute.list
timeout --signal=TERM --kill-after=10s 240s \
  apt-get -o Acquire::http::Timeout=45 -o Acquire::https::Timeout=45 update -qq

pinned_packages=(fuse3=3.18.2-1 libfuse3-4=3.18.2-1)
packages=("${pinned_packages[@]}")
case "$1" in
  fstests|fstests-build)
    pinned_packages+=(xfsprogs=6.18.0-3)
    # Compiled fstests helpers need shared libraries, not the upstream
    # autotools/header packages that the producer has already consumed.
    packages+=(xfsprogs=6.18.0-3
      acct acl attr bc e2fsprogs fio fsverity gawk
      libacl1 libaio1t64 libattr1 libcap2
      lvm2 psmisc quota sqlite3 uuid-runtime)
    if [[ "$1" == "fstests-build" ]]; then
      packages+=(autoconf automake libacl1-dev libaio-dev libcap-dev
        libtool libtool-bin uuid-dev xfslibs-dev)
    else
      # The timeout path captures gdb/strace evidence. Keep these available
      # on first failure rather than trying APT on a wedged FUSE runner.
      packages+=(gdb strace)
    fi
    ;;
  pjdfstest)
    packages+=(autoconf automake perl)
    ;;
esac

# One install avoids repeated dependency resolution and dpkg service restarts.
# Explicit versions preserve the historical xfs_io/FUSE known-gap contract.
timeout --signal=TERM --kill-after=10s 240s \
  apt-get -o Acquire::http::Timeout=45 -o Acquire::https::Timeout=45 \
    install -y -qq --no-install-recommends -t resolute "${packages[@]}"

for spec in "${pinned_packages[@]}"; do
  package="${spec%%=*}"
  expected="${spec#*=}"
  actual="$(dpkg-query -W -f='${Version}' "${package}")"
  if [[ "${actual}" != "${expected}" ]]; then
    echo "ERROR: ${package} version ${actual} differs from expected ${expected}" >&2
    exit 1
  fi
  echo "CI runtime package ${package}=${actual}"
done
fusermount3 --version
