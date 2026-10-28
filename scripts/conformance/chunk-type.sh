#!/usr/bin/env bash
# Shared chunk selection for conformance format commands. Sourced by
# pjdfstest and fstests, not invoked directly; new volumes default to Slice.

CONFORMANCE_CHUNK_TYPE="${SWORDFS_CONFORMANCE_CHUNK_TYPE:-chunk_slice}"
CONFORMANCE_FORMAT_CHUNK_ARGS=()
case "${CONFORMANCE_CHUNK_TYPE}" in
  chunk_slice)
    ;;
  cow)
    CONFORMANCE_FORMAT_CHUNK_ARGS=(--chunk-type cow)
    ;;
  *)
    echo "ERROR: unsupported conformance chunk type: ${CONFORMANCE_CHUNK_TYPE}" >&2
    return 2
    ;;
esac
