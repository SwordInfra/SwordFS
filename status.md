# SwordFS fstests conformance

Baseline gate: **BLOCKED**

A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; it does not mean every selected testcase passes.

| Metric | Value |
| --- | ---: |
| Selected upstream testcases | 645 |
| Executed testcases | 0 |
| Deferred from CI | 1 |
| Explicitly supported | 145 |
| Known gaps | 499 |
| Overall support | 0.00% |
| Supported gate | 0.00% |
| Executed population classified | 0.00% |
| Full selected population classified | 99.84% |
| Blocking outcomes | 646 |

## Raw execution outcomes

| Result | Count |
| --- | ---: |
| `DEFERRED` | 1 |

## Outcome counts

| Classification | Count |
| --- | ---: |
| `DEFERRED_CI` | 1 |
| `INFRASTRUCTURE` | 646 |

## Deferred from CI

| Test | Reason |
| --- | --- |
| `generic/478` | Explicitly unsupported OFD byte-range lock lifecycle test (#464 not planned): with FUSE remote POSIX enabled its mixed POSIX/OFD checks can hang on opaque OFD owner cleanup; no full OFD feature or conformance is claimed. Keep selected for provenance, defer costly unsupported execution from required PR-CI; never count as PASS. |

## Blocking results

| Test | Classification | Detail |
| --- | --- | --- |
| `infrastructure/1` | `INFRASTRUCTURE` | result XUnit: build/fstests-conformance/raw/results/result.xml: XUnit contains no testcases |
| `infrastructure/2` | `INFRASTRUCTURE` | fstests harness exited with status 2 |
| `generic/001` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/002` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/003` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/004` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/005` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/006` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/007` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/008` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/009` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/011` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/012` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/013` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/014` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/015` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/016` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/018` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/020` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/021` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/022` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/023` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/024` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/025` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/026` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/028` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/029` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/030` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/031` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/032` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/033` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/034` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/035` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/037` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/039` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/040` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/041` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/042` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/050` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/052` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/053` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/056` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/057` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/058` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/059` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/060` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/061` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/062` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/063` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/064` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/065` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/066` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/067` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/069` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/070` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/071` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/073` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/075` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/076` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/078` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/079` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/080` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/081` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/082` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/084` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/086` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/087` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/088` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/090` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/091` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/092` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/094` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/095` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/096` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/097` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/098` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/099` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/101` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/103` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/104` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/105` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/106` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/107` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/108` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/110` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/111` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/112` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/113` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/114` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/115` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/116` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/117` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/118` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/119` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/120` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/121` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/122` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/123` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/124` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |
| `generic/126` | `INFRASTRUCTURE` | selected testcase has no result; the fstests run is incomplete |

Only the first 100 of 646 blockers are shown; see `result.json` for all results.

## Execution metadata

- SwordFS commit: `c5ec23e8569b8b9bdc017edbdbba561b6b647051`
- fstests commit: `a370dcbed43563f0462801e889e0eceb93c7cfad`
- fstests patchset: `unknown`
- fstests selector: `generic/quick`
- Kernel: `unknown`
- libfuse: `unknown`
- Runner: `GitHub Actions (10 fstests shards)`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/40432556177
Recorded at: 2026-10-27T01:31:12+00:00

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
| 2026-10-13T13:00:13+00:00 | `8d3f92e22d59` | PASS | 22.33% | 144 | 146 | 17 | 482 | 0 | 501 |
| 2026-10-14T02:03:45+00:00 | `fb0086a75d7d` | PASS | 22.33% | 144 | 147 | 16 | 482 | 0 | 501 |
| 2026-10-16T04:01:30+00:00 | `20f25e1aa533` | PASS | 22.33% | 144 | 146 | 17 | 482 | 0 | 501 |
| 2026-10-19T09:01:17+00:00 | `b1426cb86477` | PASS | 22.48% | 145 | 145 | 17 | 482 | 1 | 499 |
| 2026-10-20T09:46:38+00:00 | `092dc272d50d` | BLOCKED | 18.45% | 145 | 119 | 17 | 482 | 1 | 499 |
| 2026-10-20T12:02:38+00:00 | `67ad6ee6881b` | PASS | 22.48% | 145 | 145 | 17 | 482 | 1 | 499 |
| 2026-10-27T01:31:12+00:00 | `c5ec23e8569b` | BLOCKED | 0.00% | 145 | 0 | 0 | 0 | 1 | 499 |

Last fully healthy baseline: `9fce9cd353d7ad4b81aea66f35718d26b85c5b99`
