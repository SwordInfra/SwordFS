# Issue #321: Pin the shared MinIO CI image

## Problem

SwordFS E2E, pjdfstest, and fstests all use `docker-compose.e2e.yml`. The MinIO service previously used `quay.io/minio/minio:latest`. The mutable Quay `latest` reference is no longer resolvable by current GitHub Actions runners. A follow-up attempt to pin the final Community Edition release on Quay also failed because that Quay manifest is gone. Docker Hub still publishes the official multi-platform image for that release, so use its immutable index digest instead of another registry/tag pair.

## Design

Pin the default shared test dependency to:

`minio/minio@sha256:14cea493d9a34af32f524e538b8346cf79f3321eff8e708c1e2960462bd8936e`

The digest identifies the official Docker Hub multi-platform index for `RELEASE.2025-09-07T16-13-09Z`. Keep `MINIO_IMAGE` as an environment override so controlled experiments can select another image without editing the repository.

This is a CI dependency reproducibility fix only. It does not change SwordFS storage semantics, credentials, ports, health checks, or bucket setup.

## Verification

Static compose/YAML/pre-commit checks run locally. GitHub CI is authoritative: E2E, pjdfstest, and fstests must successfully start MinIO and proceed into their real test phases.
