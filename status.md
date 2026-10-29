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

- SwordFS commit: `87b878100fe3130b7af4019520f04e5b35e58442`
- pjdfstest commit: `85a8aea9e685999ef0540392fd80535f873d7ff7`
- chunk_type: `chunk_slice`
- kernel: `7.0.0-1017-azure`
- libfuse: `fusermount3 version: 3.18.2`
- os: `Linux-7.0.0-1017-azure-x86_64-with-glibc2.43`
- runner: `Linux`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/40804825940
Recorded at: 2026-10-29T02:44:03+00:00

Mechanism: `chunk_slice` (previous COW history retained separately in `history.json`)

## Historical main-branch trend (chunk_slice only)

| Time | SwordFS | Status | Overall support | Supported gate | Classified |
| --- | --- | --- | ---: | ---: | ---: |
| 2026-10-28T08:45:42+00:00 | `493c1268a2e8` | PASS | 100.00% | 100.00% | 100.00% |

Last fully healthy baseline: `87b878100fe3130b7af4019520f04e5b35e58442`
