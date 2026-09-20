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
- `codecov/patch`

A required check that fails, is cancelled, is still pending, or does not report
success keeps the ruleset unsatisfied and prevents a normal merge to `main`.
Strict required-check mode also requires the PR head to be tested against the
current target branch state.

`publish-pjdfstest-status` is intentionally not required because it runs only
after a push to `main`; making a post-merge job a pre-merge requirement would
make the policy impossible to satisfy.

`pjdfstest-conformance` and `publish-pjdfstest-status` remain separate jobs
primarily for least-privilege isolation. The conformance job only needs
`contents: read` while validating PR semantics and enforcing regression policy.
The publication job needs `contents: write` only after an authoritative `main`
run so it can update the `pjdfstest-status` branch. Combining them would require
granting write permission to the conformance job even on PR validation runs,
without improving the regression gate itself.

## Check identity and maintenance

Required status checks are identified by their reported GitHub context names.
When a mandatory CI job is renamed, split, removed, or added, update the
`CI-Must-Pass` ruleset in the same coordinated change. A stale required context
must not be worked around by weakening the policy.

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
