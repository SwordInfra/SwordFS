# CI merge policy

SwordFS treats mandatory CI as a merge gate rather than an advisory signal.
Pull requests targeting `main` must not be mergeable until every mandatory
validation check has completed successfully.

## Enforcement

GitHub repository rules are the enforcement mechanism. The repository ruleset
`CI-Must-Pass` targets `main` and uses GitHub's required-status-check rule.
There is no repository-owned wrapper or aggregate CI job: GitHub already knows
how to require individual check runs/status contexts before merge.

The required PR checks are:

- `clang-format`
- `dead-code-audit`
- `build-and-test (Debug)`
- `build-and-test (Release)`
- `e2e-test (Release)`
- `pjdfstest-conformance (Release)`
- `fstests-conformance (Release)`
- `codecov/patch`

A required check that fails, is cancelled, is still pending, or does not report
success keeps the ruleset unsatisfied and prevents a normal merge to `main`.
Strict required-check mode also requires the PR head to be tested against the
current target branch state.

`publish-pjdfstest-status` and `publish-fstests-status` are intentionally not
required because they run only after a push to `main`; making a post-merge
publication job a pre-merge requirement would make the policy impossible to
satisfy.

Both `pjdfstest-conformance (Release)` and `fstests-conformance (Release)` are
required semantic gates. pjdfstest protects the pathname/metadata-oriented
POSIX baseline; fstests exercises a broader Linux filesystem contract through
the persistent Redis + MinIO FUSE path. Their known-gap mechanisms may keep
validated missing capabilities visible, but unexpected regressions,
infrastructure failures, stale XPASS entries, and disappearing supported
coverage fail the corresponding required check.

Baseline changes are themselves gated. Removing an already-supported testcase
requires the explicitly reviewed `conformance-semantic-change` label. The
fstests PR-CI deferred set is non-growing by default; adding a new deferred
testcase requires `conformance-deferred-change`. These labels do not bypass the
conformance job: they only authorize the classifier to evaluate the explicitly
reviewed baseline transition. Restoring deferred coverage to normal execution
requires no override.

Each conformance gate remains separate from its status-publication job for
least-privilege isolation. `pjdfstest-conformance` and `fstests-conformance`
need only `contents: read` while validating PR semantics and enforcing
regression policy. Their publication jobs need `contents: write` only after an
authoritative `main` run so they can update `pjdfstest-status` and
`fstests-status`. Combining these responsibilities would grant write permission
to conformance jobs on PR validation runs without improving the regression gate.

## Development-only CI

Developer feedback that replaces local SwordFS compilation is intentionally
separate from the formal merge gate.

The development workflow is manually dispatched and may accept narrow inputs
such as CMake build type, build target, and an optional unit-test filter. Its
purpose is to let a developer push a task branch and compile or run only the
focused target needed for the current iteration without starting the complete
PR/main CI matrix.

Development-only jobs:

- run only from an explicit manual dispatch;
- do not run from normal `push`, `pull_request`, or `main` CI events;
- are not listed in the `CI-Must-Pass` required-status checks;
- are not merge-readiness evidence by themselves;
- should reuse the same dependency installation/cache paths and CMake presets
  as formal CI where practical, so the two execution paths do not drift.

The formal jobs in `.github/workflows/ci.yml` remain authoritative for pull
request readiness, full Debug/Release verification, coverage, E2E, pjdfstest,
fstests, and the protected-branch merge gate. A successful development-only
build never substitutes for those checks.

After `.github/workflows/dev-build.yml` exists on the default branch, run it
against a task branch with GitHub CLI. For example, a focused Debug unit-test
iteration can use:

```bash
gh workflow run dev-build.yml \
  --ref <task-branch> \
  -f build_type=Debug \
  -f target=swordfs_test \
  -f run_unit_tests=true \
  -f gtest_filter='SuiteName.TestName'
```

Omit `run_unit_tests` and `gtest_filter` when only compilation is needed.
Selecting `swordfs` or `swordfs_e2e_test` builds that target without running
the corresponding integration/E2E workload.

## Check identity and maintenance

Required status checks are identified by their reported GitHub context names.
When a mandatory CI job is renamed, split, removed, or added, update the
`CI-Must-Pass` ruleset in the same coordinated change. A stale required context
must not be worked around by weakening the policy.

There is one required activation-order constraint when introducing a brand-new
check context: do **not** add that context to the repository ruleset before the
workflow defining it has landed on the default branch. Otherwise unrelated PRs
can be blocked by a required check that their target branch cannot produce.
The introducing PR must run the new job and treat it as a manual merge gate;
immediately after that PR lands, add the new context to `CI-Must-Pass` and
verify the live ruleset. This transition rule does not make the new job
optional; it prevents an impossible required-check state during rollout.

GitHub Actions checks should be bound to the GitHub Actions app when the
ruleset supports an integration ID. External checks such as `codecov/patch`
remain bound to their reporting integration/status context.

## Bypass policy

The normal development path has no bypass actor. A failed required check is a
reason to fix the change or CI, not to bypass the gate. If the repository later
introduces an emergency administrative bypass, that policy must be explicit,
narrow, and auditable.

## Verification

Repository-policy changes require live GitHub verification rather than a local
unit-test surrogate:

1. confirm the ruleset is active and targets `main`;
2. confirm every mandatory check above appears in its required-status list;
3. use a temporary PR with an intentional CI failure to prove GitHub blocks
   merge while the required check is red;
4. confirm a healthy PR becomes mergeable once all required checks are green;
5. close the temporary verification PR without merging it.

The individual CI jobs remain responsible for their own test, analysis,
coverage, and conformance semantics. This policy only makes their results
authoritative at merge time.
