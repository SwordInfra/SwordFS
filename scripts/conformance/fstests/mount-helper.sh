#!/usr/bin/env bash
# mount(8) helper used by upstream fstests when FSTYP=fuse.
set -euo pipefail

CONFIG_FILE="${SWORDFS_FSTESTS_MOUNT_CONFIG:-/tmp/swordfs-fstests-mount.env}"
if [[ ! -r "${CONFIG_FILE}" ]]; then
  echo "ERROR: missing SwordFS fstests mount-helper config: ${CONFIG_FILE}" >&2
  exit 2
fi

# shellcheck disable=SC1090
source "${CONFIG_FILE}"

if [[ $# -lt 2 ]]; then
  echo "usage: mount.fuse.swordfs <device> <mountpoint> [-o options]" >&2
  exit 2
fi

device="$1"
mountpoint="$2"
shift 2

case "${device}" in
  "${FSTESTS_TEST_DEV}")
    volume="${FSTESTS_TEST_VOLUME}"
    log_file="${FSTESTS_LOG_DIR}/swordfs-test.log"
    pid_file="${FSTESTS_LOG_DIR}/swordfs-test.pid"
    ;;
  "${FSTESTS_SCRATCH_DEV}")
    volume="${FSTESTS_SCRATCH_VOLUME}"
    log_file="${FSTESTS_LOG_DIR}/swordfs-scratch.log"
    pid_file="${FSTESTS_LOG_DIR}/swordfs-scratch.pid"
    ;;
  *)
    echo "ERROR: unknown SwordFS fstests device: ${device}" >&2
    exit 2
    ;;
esac

# fstests passes ordinary mount(8) options through the FUSE helper. Preserve
# them so tests that intentionally exercise mount options observe the same
# request. allow_other is mandatory for fstests' fsgqa users.
fuse_options=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    -o)
      [[ $# -ge 2 ]] || { echo "ERROR: -o requires an argument" >&2; exit 2; }
      fuse_options+="${fuse_options:+,}$2"
      shift 2
      ;;
    -o*)
      fuse_options+="${fuse_options:+,}${1#-o}"
      shift
      ;;
    *)
      # mount helpers can receive generic flags that libfuse does not consume.
      # fstests' FUSE path expresses the filesystem options through -o.
      shift
      ;;
  esac
done

case ",${fuse_options}," in
  *,allow_other,*) ;;
  *) fuse_options="allow_other${fuse_options:+,${fuse_options}}" ;;
esac

# fstests treats TEST_DEV/SCRATCH_DEV as the mount source identity and checks
# that identity with df/findmnt after invoking the helper. Without an explicit
# fsname, libfuse exposes /dev/fuse as the source and fstests rejects an
# otherwise healthy mount before testcase selection starts.
fuse_options="${fuse_options:+${fuse_options},}fsname=${device}"

SWORDFS_S3_NO_SSL=1 \
AWS_DEFAULT_REGION=auto \
AWS_ACCESS_KEY_ID="${FSTESTS_MINIO_ROOT_USER}" \
AWS_SECRET_ACCESS_KEY="${FSTESTS_MINIO_ROOT_PASSWORD}" \
"${FSTESTS_SWORDFS_BIN}" --log-file "${log_file}" mount \
  --volume "${volume}" \
  --meta "${FSTESTS_METADATA_URL}" \
  --fuse-threads 4 \
  --storage-thread-count 4 \
  --pidfile "${pid_file}" \
  -o "${fuse_options}" \
  "${mountpoint}"
