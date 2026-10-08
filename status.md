# SwordFS fstests conformance

Baseline gate: **PASS**

A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; it does not mean every selected testcase passes.

| Metric | Value |
| --- | ---: |
| Selected upstream testcases | 645 |
| Executed testcases | 645 |
| Deferred from CI | 0 |
| Explicitly supported | 144 |
| Known gaps | 501 |
| Overall support | 22.33% |
| Supported gate | 100.00% |
| Executed population classified | 100.00% |
| Full selected population classified | 100.00% |
| Blocking outcomes | 0 |

## Raw execution outcomes

| Result | Count |
| --- | ---: |
| `PASS` | 144 |
| `FAIL` | 19 |
| `NOTRUN` | 482 |

## Outcome counts

| Classification | Count |
| --- | ---: |
| `ENVIRONMENT` | 8 |
| `KNOWN_UNSUPPORTED` | 90 |
| `PASS` | 144 |
| `UPSTREAM_NOT_APPLICABLE` | 402 |
| `UPSTREAM_TEST_DEFECT` | 1 |

## Known-gap Issues

- [#255](https://github.com/SwordInfra/SwordFS/issues/255): 89 testcase(s), KNOWN_UNSUPPORTED
- [#406](https://github.com/SwordInfra/SwordFS/issues/406): 1 testcase(s), KNOWN_UNSUPPORTED

## Execution metadata

- SwordFS commit: `e7bd4247fe7fce2a2451c9a04d6a114b6996be8d`
- fstests commit: `a370dcbed43563f0462801e889e0eceb93c7cfad`
- fstests patchset: `conformance/fstests/patches/generic-615-graceful-stat-loop-exit.patch@sha256:863335954392d7a6343c8dd4f1a2decb9bab59c8bb65ac83a2f748d930f8ae19`
- fstests selector: `generic/quick`
- Kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- Runner: `GitHub Actions (9 fstests shards)`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/37740343878
Recorded at: 2026-10-08T07:17:46+00:00

## Historical main-branch progress

| Time | SwordFS | Status | Overall support | Supported | PASS | FAIL | NOTRUN | Deferred | Known gaps |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2026-09-22T15:17:08+00:00 | `00c7d60ed734` | PASS | n/a | 102 | 102 | 25 | 515 | 3 | 540 |
| 2026-09-23T02:42:08+00:00 | `d8e33622d4f3` | PASS | 15.81% | 102 | 102 | 25 | 515 | 3 | 540 |
| 2026-09-23T07:58:39+00:00 | `ae2a1c12b28f` | PASS | 15.97% | 103 | 103 | 24 | 515 | 3 | 539 |
| 2026-09-23T10:03:17+00:00 | `d8762eb140d4` | PASS | 16.43% | 106 | 106 | 21 | 515 | 3 | 536 |
| 2026-09-25T02:13:31+00:00 | `d7a44b696245` | PASS | 16.74% | 108 | 108 | 22 | 515 | 0 | 537 |
| 2026-09-25T05:26:32+00:00 | `1970dcdcd2de` | BLOCKED | 12.56% | 109 | 81 | 21 | 515 | 0 | 536 |
| 2026-09-25T07:48:50+00:00 | `485132b5d9bd` | PASS | 16.90% | 109 | 109 | 21 | 515 | 0 | 536 |
| 2026-09-25T14:54:21+00:00 | `33aa7f676b8d` | PASS | 17.05% | 110 | 110 | 20 | 515 | 0 | 535 |
| 2026-09-26T12:49:23+00:00 | `6f426b5ac78c` | PASS | 17.21% | 111 | 111 | 19 | 515 | 0 | 534 |
| 2026-09-28T02:46:14+00:00 | `b2024540db14` | PASS | 17.36% | 112 | 112 | 18 | 515 | 0 | 533 |
| 2026-09-28T07:37:50+00:00 | `20d61ea022ad` | PASS | 17.52% | 113 | 113 | 17 | 515 | 0 | 532 |
| 2026-09-29T02:43:18+00:00 | `32929c97aae0` | PASS | 19.38% | 125 | 125 | 17 | 503 | 0 | 520 |
| 2026-09-29T05:04:11+00:00 | `9494348d5367` | PASS | 19.53% | 126 | 126 | 17 | 502 | 0 | 519 |
| 2026-09-30T01:30:44+00:00 | `9d7112bed869` | PASS | 20.16% | 130 | 130 | 18 | 497 | 0 | 515 |
| 2026-09-30T08:18:42+00:00 | `b3e2b7769f53` | PASS | 21.86% | 141 | 141 | 19 | 485 | 0 | 504 |
| 2026-10-08T02:06:05+00:00 | `0ff0a31b5d43` | PASS | 22.33% | 144 | 144 | 19 | 482 | 0 | 501 |

Last fully healthy baseline: `e7bd4247fe7fce2a2451c9a04d6a114b6996be8d`
