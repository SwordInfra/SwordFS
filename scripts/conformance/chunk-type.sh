#!/usr/bin/env bash
# Shared opt-in chunk selection for conformance format commands. Sourced by
# pjdfstest and fstests, not invoked directly; keep default COW CLI identical.

CONFORMANCE_CHUNK_TYPE="${SWORDFS_CONFORMANCE_CHUNK_TYPE:-cow}"
CONFORMANCE_FORMAT_CHUNK_ARGS=()
case "${CONFORMANCE_CHUNK_TYPE}" in
  cow)
    ;;
  chunk_slice)
    CONFORMANCE_FORMAT_CHUNK_ARGS=(--chunk-type chunk_slice)
    ;;
  *)
    echo "ERROR: unsupported conformance chunk type: ${CONFORMANCE_CHUNK_TYPE}" >&2
    return 2
    ;;
esac
