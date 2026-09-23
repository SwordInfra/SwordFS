# SwordFS fstests conformance

Baseline gate: **PASS**

A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; it does not mean every selected testcase passes.

| Metric | Value |
| --- | ---: |
| Selected upstream testcases | 645 |
| Executed testcases | 642 |
| Deferred from CI | 3 |
| Explicitly supported | 102 |
| Known gaps | 540 |
| Overall support | 15.81% |
| Supported gate | 100.00% |
| Executed population classified | 100.00% |
| Full selected population classified | 99.53% |
| Blocking outcomes | 0 |

## Raw execution outcomes

| Result | Count |
| --- | ---: |
| `PASS` | 102 |
| `FAIL` | 25 |
| `NOTRUN` | 515 |
| `DEFERRED` | 3 |

## Outcome counts

| Classification | Count |
| --- | ---: |
| `DEFERRED_CI` | 3 |
| `ENVIRONMENT` | 7 |
| `KNOWN_SEMANTIC_DEFECT` | 9 |
| `KNOWN_UNSUPPORTED` | 141 |
| `PASS` | 102 |
| `UPSTREAM_NOT_APPLICABLE` | 383 |

## Deferred from CI

| Test | Reason |
| --- | --- |
| `generic/069` | Tracked by #248; observed by ZeroFS to take about 16 minutes on FUSE, so O_APPEND correctness remains unclaimed until moved into bounded executed coverage. |
| `generic/471` | Tracked by #248; FUSE rewinddir/readdir case reported by ZeroFS/fuser as CI-prohibitive, so defer from PR gate without counting it as supported. |
| `generic/478` | Tracked by #248; OFD-lock test can hang in the blocking-lock path on FUSE, so defer until lock behavior is exercised in bounded executed coverage. |

## Known-gap Issues

- [#255](https://github.com/SwordInfra/SwordFS/issues/255): 141 testcase(s), KNOWN_UNSUPPORTED
- [#256](https://github.com/SwordInfra/SwordFS/issues/256): 9 testcase(s), KNOWN_SEMANTIC_DEFECT

## Execution metadata

- SwordFS commit: `3d6cdd0871fa88c62a5cd8ee84b27972ef1d4765`
- fstests commit: `a370dcbed43563f0462801e889e0eceb93c7cfad`
- fstests selector: `generic/quick`
- Kernel: `6.17.0-1022-azure`
- libfuse: `fusermount3 version: 3.18.2`
- Runner: `GitHub Actions (6 fstests shards)`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/35813137163
Recorded at: 2026-09-23T03:21:14+00:00

## Historical main-branch progress

| Time | SwordFS | Status | Overall support | Supported | PASS | FAIL | NOTRUN | Deferred | Known gaps |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2026-09-22T15:17:08+00:00 | `00c7d60ed734` | PASS | n/a | 102 | 102 | 25 | 515 | 3 | 540 |
| 2026-09-23T02:42:08+00:00 | `d8e33622d4f3` | PASS | 15.81% | 102 | 102 | 25 | 515 | 3 | 540 |

Last fully healthy baseline: `3d6cdd0871fa88c62a5cd8ee84b27972ef1d4765`
