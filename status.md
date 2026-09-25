# SwordFS POSIX conformance status

Latest run: **PASS**

| Metric | Value |
| --- | ---: |
| Overall conformance support | 100.00% |
| Supported regression pass | 100.00% |
| Classified rate | 100.00% |
| Supported assertion count | 8770 |
| Blocking result count | 0 |

## Classification counts

| Classification | Count |
| --- | ---: |
| PASS | 8770 |
| UPSTREAM_NOT_APPLICABLE | 28 |

## Per-category support

| Category | PASS | Applicable | Support |
| --- | ---: | ---: | ---: |
| chflags | 14 | 14 | 100.00% |
| chmod | 327 | 327 | 100.00% |
| chown | 1470 | 1470 | 100.00% |
| ftruncate | 89 | 89 | 100.00% |
| granular | 7 | 7 | 100.00% |
| link | 359 | 359 | 100.00% |
| mkdir | 118 | 118 | 100.00% |
| mkfifo | 120 | 120 | 100.00% |
| mknod | 186 | 186 | 100.00% |
| open | 337 | 337 | 100.00% |
| posix_fallocate | 1 | 1 | 100.00% |
| rename | 4857 | 4857 | 100.00% |
| rmdir | 145 | 145 | 100.00% |
| symlink | 95 | 95 | 100.00% |
| truncate | 84 | 84 | 100.00% |
| unlink | 439 | 439 | 100.00% |
| utimensat | 122 | 122 | 100.00% |

## Baseline metadata

- SwordFS commit: `4ca168d27b2cf47cc044df3561470fb3b5e6d613`
- pjdfstest commit: `85a8aea9e685999ef0540392fd80535f873d7ff7`
- kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- os: `Linux-6.17.0-1022-azure-x86_64-with-glibc2.43`
- runner: `Linux`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/36089395252
Recorded at: 2026-09-25T03:24:09+00:00

## Historical main-branch trend

| Time | SwordFS | Status | Overall support | Supported gate | Classified |
| --- | --- | --- | ---: | ---: | ---: |
| 2026-09-20T08:18:31+00:00 | `e844e9a8df64` | PASS | 57.14% | 100.00% | 100.00% |
| 2026-09-20T13:53:45+00:00 | `4ad8c24be061` | PASS | 57.16% | 100.00% | 100.00% |
| 2026-09-21T01:48:15+00:00 | `8deedcedadd0` | PASS | 58.91% | 100.00% | 100.00% |
| 2026-09-21T08:03:26+00:00 | `a2eeba4b9d3c` | PASS | 59.05% | 100.00% | 100.00% |
| 2026-09-21T09:39:49+00:00 | `7bc62e977b35` | PASS | 100.00% | 100.00% | 100.00% |

Last fully healthy baseline: `4ca168d27b2cf47cc044df3561470fb3b5e6d613`
