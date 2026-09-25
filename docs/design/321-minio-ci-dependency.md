# Issue #321: Make the shared MinIO CI dependency reproducible

## Problem

SwordFS E2E, pjdfstest, and fstests used a registry-backed MinIO service from `docker-compose.e2e.yml`. The mutable Quay `latest` reference disappeared. CI then proved that both an explicit final-release tag on Quay and the former official Docker Hub repository are no longer pullable. All MinIO-backed test surfaces consequently failed before SwordFS behavior executed.

A CI dependency whose distribution reference can disappear independently of the pinned SwordFS revision is not reproducible infrastructure.

## Design

Use official MinIO GitHub Release assets instead of a container registry:

- server: `RELEASE.2025-09-07T16-13-09Z`;
- client (`mc`): `RELEASE.2025-08-13T08-35-41Z`.

`scripts/testing/minio-test.sh` owns the shared lifecycle. It selects the amd64/arm64 release asset, verifies a pinned SHA256 from GitHub release metadata, starts MinIO directly on the runner, waits for the readiness endpoint, creates buckets through the pinned `mc`, and performs bounded shutdown.

Ephemeral MinIO object data, PID state, and `mc` configuration live under a private `/tmp/swordfs-minio-test-*` runtime directory. Evidence directories retain only `minio.log`, so a cancelled root-run conformance job cannot make artifact collection fail on unreadable runtime state.

The CI workflow has one `minio-test-tools` job that downloads and verifies the two binaries once and publishes them as a short-lived workflow artifact. Debug service-backed coverage, E2E, pjdfstest, and fstests download that artifact instead of hitting the external release endpoint independently. The helper retains the same verified-download path as a fallback for developer runs outside CI.

Docker Compose now owns Redis only. The tested SwordFS topology remains Redis metadata plus MinIO-compatible S3 on `127.0.0.1:9000`; only dependency distribution and process lifecycle change.

## Integrity and reproducibility

Pinned release assets are accepted only when their SHA256 matches:

- MinIO linux-amd64: `7c5bd8512c6e966455b1d198209358b2d191c77a83ab377c4073281065fb855f`;
- MinIO linux-arm64: `5c83cd2cf151717ba0243f73e1c7802ff36e272b67144bdd7f1f7d684fd6f03d`;
- mc linux-amd64: `01f866e9c5f9b87c2b09116fa5d7c06695b106242d829a8bb32990c00312e891`;
- mc linux-arm64: `14c8c9616cfce4636add161304353244e8de383b2e2752c0e9dad01d4c27c12c`.

No third-party MinIO image is introduced.

## E2E process-identity interaction

Running MinIO directly exposes its `/tmp/swordfs-minio-test-*` runtime path in the MinIO process command line. The historical `StaleMountTest` helper treated any `/proc/<pid>/cmdline` containing the substring `swordfs` as a SwordFS daemon, so it incorrectly counted the MinIO process and failed two daemon-exit assertions even though the fixture-owned SwordFS daemon had already exited.

The stale-mount shutdown assertion therefore uses the existing `Fixture::IsDaemonGone()` contract, which tracks the exact daemon PID created by that fixture and waits boundedly for that process to disappear. Global substring-based process discovery is removed. This is a test synchronization correction required by the runner-owned MinIO topology; production mount behavior is unchanged.

## Verification

Local verification covers shell/YAML/pre-commit, Redis-only Compose rendering, and helper structure. GitHub CI is authoritative for GitHub Release asset retrieval, MinIO startup, bucket creation, and the real Debug/E2E/pjdfstest/fstests execution surfaces.


## Startup readiness

The helper must wait on `/minio/health/ready`, not only `/minio/health/live`. CI evidence from fstests showed that the liveness endpoint can succeed before the object layer is initialized, allowing the first `mc alias set` request to race startup with `Server not initialized yet`. Readiness is therefore the contract for handing MinIO to callers.
