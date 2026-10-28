# SwordFS fstests conformance

Baseline gate: **PASS**

A passing baseline gate means the admitted supported/known-gap contract has no blocking regression; it does not mean every selected testcase passes.

| Metric | Value |
| --- | ---: |
| Selected upstream testcases | 645 |
| Executed testcases | 644 |
| Deferred from CI | 1 |
| Explicitly supported | 145 |
| Known gaps | 499 |
| Overall support | 22.48% |
| Supported gate | 100.00% |
| Executed population classified | 100.00% |
| Full selected population classified | 99.84% |
| Blocking outcomes | 0 |

## Raw execution outcomes

| Result | Count |
| --- | ---: |
| `PASS` | 145 |
| `FAIL` | 17 |
| `NOTRUN` | 482 |
| `DEFERRED` | 1 |

## Outcome counts

| Classification | Count |
| --- | ---: |
| `DEFERRED_CI` | 1 |
| `ENVIRONMENT` | 8 |
| `KNOWN_UNSUPPORTED` | 87 |
| `PASS` | 145 |
| `UPSTREAM_NOT_APPLICABLE` | 403 |
| `UPSTREAM_TEST_DEFECT` | 1 |

## Deferred from CI

| Test | Reason |
| --- | --- |
| `generic/478` | Explicitly unsupported OFD byte-range lock lifecycle test (#464 not planned): with FUSE remote POSIX enabled its mixed POSIX/OFD checks can hang on opaque OFD owner cleanup; no full OFD feature or conformance is claimed. Keep selected for provenance, defer costly unsupported execution from required PR-CI; never count as PASS. |

## Known-gap Issues

- [#255](https://github.com/SwordInfra/SwordFS/issues/255): 86 testcase(s), KNOWN_UNSUPPORTED
- [#406](https://github.com/SwordInfra/SwordFS/issues/406): 1 testcase(s), KNOWN_UNSUPPORTED

## Execution metadata

- SwordFS commit: `e7b28617edd4a562b72102e65d68aa62ee9f197a`
- fstests commit: `a370dcbed43563f0462801e889e0eceb93c7cfad`
- fstests patchset: `conformance/fstests/patches/generic-615-graceful-stat-loop-exit.patch@sha256:863335954392d7a6343c8dd4f1a2decb9bab59c8bb65ac83a2f748d930f8ae19`
- fstests selector: `generic/quick`
- Kernel: `multiple (see shard-environments.json)`
- libfuse: `fusermount3 version: 3.18.2`
- Runner: `GitHub Actions (10 fstests shards)`

Authoritative CI run: https://github.com/SwordInfra/SwordFS/actions/runs/40663157666
Recorded at: 2026-10-28T09:31:51+00:00

Mechanism: `chunk_slice` (previous COW history retained separately in `history.json`)

## Historical main-branch progress (chunk_slice only)

| Time | SwordFS | Status | Overall support | Supported | PASS | FAIL | NOTRUN | Deferred | Known gaps |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2026-10-28T08:51:54+00:00 | `493c1268a2e8` | PASS | 22.48% | 145 | 145 | 17 | 482 | 1 | 499 |

Last fully healthy baseline: `e7b28617edd4a562b72102e65d68aa62ee9f197a`
