# SwordFS fstests conformance

Baseline gate: **BLOCKED**

A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; it does not mean every selected testcase passes.

| Metric | Value |
| --- | ---: |
| Selected upstream testcases | 645 |
| Executed testcases | 378 |
| Deferred from CI | 0 |
| Explicitly supported | 111 |
| Known gaps | 534 |
| Overall support | 17.21% |
| Supported gate | 100.00% |
| Executed population classified | 100.00% |
| Full selected population classified | 100.00% |
| Blocking outcomes | 268 |

## Raw execution outcomes

| Result | Count |
| --- | ---: |
| `PASS` | 111 |
| `FAIL` | 10 |
| `NOTRUN` | 257 |

## Outcome counts

| Classification | Count |
| --- | ---: |
| `ENVIRONMENT` | 3 |
| `INFRASTRUCTURE` | 268 |
| `KNOWN_SEMANTIC_DEFECT` | 1 |
| `KNOWN_UNSUPPORTED` | 62 |
| `PASS` | 111 |
| `UPSTREAM_NOT_APPLICABLE` | 200 |
| `UPSTREAM_TEST_DEFECT` | 1 |

## Blocking results

| Test | Classification | Detail |
| --- | --- | --- |
| `infrastructure/1` | `INFRASTRUCTURE` | fstests harness exited with status 2 |
| `generic/003` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/008` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/012` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/016` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/020` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/022` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/031` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/033` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/037` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/040` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/042` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/052` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/056` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/058` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/060` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/062` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/064` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/066` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/070` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/073` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/078` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/081` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/086` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/092` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/096` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/099` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/103` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/105` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/107` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/110` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/114` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/116` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/118` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/121` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/131` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/136` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/139` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/142` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/144` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/146` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/148` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/150` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/152` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/154` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/156` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/158` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/160` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/162` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/171` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/173` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/177` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/179` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/181` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/183` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/185` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/189` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/191` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/195` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/197` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/200` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/202` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/205` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/211` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/214` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/217` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/219` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/222` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/225` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/228` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/230` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/237` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/240` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/250` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/253` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/255` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/259` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/261` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/264` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/266` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/268` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/272` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/277` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/279` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/282` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/284` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/287` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/289` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/291` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/293` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/295` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/301` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/303` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/305` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/307` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/315` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/318` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/321` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/324` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/326` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |

Only the first 100 of 268 blockers are shown; see `result.json` for all results.

## Known-gap Issues

- [#255](https://github.com/SwordInfra/SwordFS/issues/255): 62 testcase(s), KNOWN_UNSUPPORTED
- [#256](https://github.com/SwordInfra/SwordFS/issues/256): 1 testcase(s), KNOWN_SEMANTIC_DEFECT

## Execution metadata

- SwordFS commit: `3b6e4f2b4632e14d260ee37a57ed34557b841acd`
- fstests commit: `a370dcbed43563f0462801e889e0eceb93c7cfad`
- fstests selector: `generic/quick`
- Kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- Runner: `GitHub Actions (9 fstests shards)`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/36325323497
Recorded at: 2026-09-27T14:31:14+00:00

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
| 2026-09-27T14:31:14+00:00 | `3b6e4f2b4632` | BLOCKED | 17.21% | 111 | 111 | 10 | 257 | 0 | 534 |

Last fully healthy baseline: `25743b96d4eb5052b72040dfbae08213b1507b06`
