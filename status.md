# SwordFS fstests conformance

Baseline gate: **PASS**

A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; it does not mean every selected testcase passes.

| Metric | Value |
| --- | ---: |
| Selected upstream testcases | 645 |
| Executed testcases | 645 |
| Deferred from CI | 0 |
| Explicitly supported | 110 |
| Known gaps | 535 |
| Overall support | 17.05% |
| Supported gate | 100.00% |
| Executed population classified | 100.00% |
| Full selected population classified | 100.00% |
| Blocking outcomes | 0 |

## Raw execution outcomes

| Result | Count |
| --- | ---: |
| `PASS` | 110 |
| `FAIL` | 20 |
| `NOTRUN` | 515 |

## Outcome counts

| Classification | Count |
| --- | ---: |
| `ENVIRONMENT` | 7 |
| `KNOWN_SEMANTIC_DEFECT` | 4 |
| `KNOWN_UNSUPPORTED` | 142 |
| `PASS` | 110 |
| `UPSTREAM_NOT_APPLICABLE` | 382 |

## Known-gap Issues

- [#255](https://github.com/SwordInfra/SwordFS/issues/255): 142 testcase(s), KNOWN_UNSUPPORTED
- [#256](https://github.com/SwordInfra/SwordFS/issues/256): 4 testcase(s), KNOWN_SEMANTIC_DEFECT

## Execution metadata

- SwordFS commit: `42e440c9ffac38e5c27807bb29dd136fe85f9545`
- fstests commit: `a370dcbed43563f0462801e889e0eceb93c7cfad`
- fstests selector: `generic/quick`
- Kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- Runner: `GitHub Actions (9 fstests shards)`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/36211926746
Recorded at: 2026-09-26T02:46:37+00:00

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

Last fully healthy baseline: `42e440c9ffac38e5c27807bb29dd136fe85f9545`
