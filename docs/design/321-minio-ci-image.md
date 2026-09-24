# Issue #321: Pin the shared MinIO CI image

## Problem

SwordFS E2E, pjdfstest, and fstests all use `docker-compose.e2e.yml`. The MinIO service previously used `quay.io/minio/minio:latest`. The mutable `latest` reference is no longer resolvable by current GitHub Actions runners, so every MinIO-backed test surface fails before executing SwordFS behavior.

## Design

Pin the default shared test dependency to:

`quay.io/minio/minio:RELEASE.2025-09-07T16-13-09Z`

Keep `MINIO_IMAGE` as an environment override so controlled experiments can select another image without editing the repository.

This is a CI dependency reproducibility fix only. It does not change SwordFS storage semantics, credentials, ports, health checks, or bucket setup.

## Verification

Static compose/YAML/pre-commit checks run locally. GitHub CI is authoritative: E2E, pjdfstest, and fstests must successfully start MinIO and proceed into their real test phases.
