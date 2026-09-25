# SwordFS fstests conformance

Baseline gate: **BLOCKED**

A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; it does not mean every selected testcase passes.

| Metric | Value |
| --- | ---: |
| Selected upstream testcases | 645 |
| Executed testcases | 617 |
| Deferred from CI | 0 |
| Explicitly supported | 109 |
| Known gaps | 536 |
| Overall support | 12.56% |
| Supported gate | 74.31% |
| Executed population classified | 100.00% |
| Full selected population classified | 100.00% |
| Blocking outcomes | 29 |

## Raw execution outcomes

| Result | Count |
| --- | ---: |
| `PASS` | 81 |
| `FAIL` | 21 |
| `NOTRUN` | 515 |

## Outcome counts

| Classification | Count |
| --- | ---: |
| `ENVIRONMENT` | 7 |
| `INFRASTRUCTURE` | 29 |
| `KNOWN_SEMANTIC_DEFECT` | 5 |
| `KNOWN_UNSUPPORTED` | 142 |
| `PASS` | 81 |
| `UPSTREAM_NOT_APPLICABLE` | 382 |

## Blocking results

| Test | Classification | Detail |
| --- | --- | --- |
| `infrastructure/1` | `INFRASTRUCTURE` | fstests harness exited with status 2 |
| `generic/006` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/007` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/013` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/025` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/084` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/087` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/126` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/169` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/221` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/248` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/249` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/309` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/313` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/346` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/360` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/401` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/430` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/437` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/519` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/598` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/604` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/637` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/676` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/678` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/708` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/736` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/761` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/799` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |

## Known-gap Issues

- [#255](https://github.com/SwordInfra/SwordFS/issues/255): 142 testcase(s), KNOWN_UNSUPPORTED
- [#256](https://github.com/SwordInfra/SwordFS/issues/256): 5 testcase(s), KNOWN_SEMANTIC_DEFECT

## Execution metadata

- SwordFS commit: `1970dcdcd2de149a38ffa81d4596dbecfedc4977`
- fstests commit: `a370dcbed43563f0462801e889e0eceb93c7cfad`
- fstests selector: `generic/quick`
- Kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- Runner: `GitHub Actions (9 fstests shards)`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/36092508129
Recorded at: 2026-09-25T05:26:32+00:00

## Historical main-branch progress

| Time | SwordFS | Status | Overall support | Supported | PASS | FAIL | NOTRUN | Deferred | Known gaps |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2026-09-22T15:17:08+00:00 | `00c7d60ed734` | PASS | n/a | 102 | 102 | 25 | 515 | 3 | 540 |
| 2026-09-23T02:42:08+00:00 | `d8e33622d4f3` | PASS | 15.81% | 102 | 102 | 25 | 515 | 3 | 540 |
| 2026-09-23T07:58:39+00:00 | `ae2a1c12b28f` | PASS | 15.97% | 103 | 103 | 24 | 515 | 3 | 539 |
| 2026-09-23T10:03:17+00:00 | `d8762eb140d4` | PASS | 16.43% | 106 | 106 | 21 | 515 | 3 | 536 |
| 2026-09-25T02:13:31+00:00 | `d7a44b696245` | PASS | 16.74% | 108 | 108 | 22 | 515 | 0 | 537 |
| 2026-09-25T05:26:32+00:00 | `1970dcdcd2de` | BLOCKED | 12.56% | 109 | 81 | 21 | 515 | 0 | 536 |

Last fully healthy baseline: `4ca168d27b2cf47cc044df3561470fb3b5e6d613`
