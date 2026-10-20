# #505 — Stable, exact fstests environment NOTRUN classification

The CI classifier requires each known-gap NOTRUN reason to match its
recorded normalized upstream text exactly. This is useful: an unexpected
skip can indicate a broken test environment or lost conformance coverage.

Pinned fstests `generic/704` and `generic/767` are exceptions only because
both require a usable block-disk fixture and `scsi_debug` support. Under
the FUSE CI environment both prerequisites are unavailable, and whichever
upstream guard executes first produces one of two recorded NOTRUN strings.
Required CI from #271 reproduced these mismatches on 2026-10-20 (runs
39406824415, 39408023144 and 39409842864):

- `generic/704`: `scsi_debug module not found` OR
  `require <FSTESTS_WORK_DIR>/test to be valid block disk`
- `generic/767`: `scsi_debug module not found` OR
  `require <FSTESTS_WORK_DIR>/scratch to be valid block disk`

**Decision:** retain the original explicit `environment` baseline rows
and allow exactly one additional normalized string for each of these
two named testcases, only when the baseline expected message is
`scsi_debug module not found`. No fuzzy regex, general block-disk
normalization, category promotion, result conversion, or classifier
gate override is permitted.

The alternate is accepted only for an actual **NOTRUN** result; PASS
remains XPASS and FAIL remains a baseline result mismatch. All other
testcases and all unrelated reasons must match their baseline exactly.
Focused classifier tests assert the two allowed alternatives plus
incorrect-path, wrong-test, unexpected-reason and unexpected-result
rejection.

This is a CI tooling change only; Slice I/O and supported fstests
classification are unaffected. The required formal fstests CI gate
must pass before #505 or its dependent #271 task can merge.
