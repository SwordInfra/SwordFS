# fstests conformance

SwordFS uses upstream `fstests` (formerly `xfstests`) as the second filesystem
conformance layer after `pjdfstest`. The two suites protect different parts of
the contract:

```text
pjdfstest -> pathname and metadata-oriented POSIX syscall semantics
fstests   -> broader Linux filesystem IO, cache, durability, allocation,
             locking, race, and generic VFS semantics
```

The fstests gate is intentionally a baseline-and-regression system rather than
a requirement that every upstream testcase already pass. SwordFS is still
expanding filesystem capability, so known semantic gaps remain visible while
working behavior is protected from regression.

## Upstream selection

`conformance/fstests/version.env` pins the canonical upstream repository,
commit, selector, and suite timeout. The runner fetches exactly that revision
and rejects a checkout whose `HEAD` does not match the configured commit.

The initial population is selected by upstream fstests itself:

```text
FSTYP=fuse
./check -fuse -g generic/quick
```

SwordFS does not keep a project-owned list of tests to execute. Before the real
run, the harness invokes the same selector with fstests' `-n` mode and XUnit
reporting. That dry run records the exact testcase population chosen by the
pinned upstream revision. The strict classifier then requires the real XUnit
result to contain exactly that population. Missing or additional testcase
results are infrastructure failures.

This separation is an anti-gaming invariant: `supported.txt` describes the
regression contract, but it never decides which upstream tests are executed.
Changing the upstream commit or `FSTESTS_GROUP` therefore makes the selected
population change visible in review and in the next classified run.

### Peer check against ZeroFS

ZeroFS provides a useful peer implementation because it also runs fstests over
a userspace/object-backed filesystem and exercises a FUSE client in CI. Its
FUSE workflow uses the same integration shape selected for SwordFS:

- `FSTYP=fuse` plus a filesystem-specific `FUSE_SUBTYP`;
- a `mount.fuse.*` helper so fstests can mount/remount the filesystem itself;
- independent TEST and SCRATCH filesystem instances; and
- the upstream quick population rather than a project-owned pass-only list.

That comparison validates using upstream's existing FUSE path instead of
adding a SwordFS-specific fstests filesystem type.

SwordFS intentionally differs from ZeroFS in baseline handling. ZeroFS keeps a
handwritten exclude file that mixes protocol/FUSE limitations, environment
quirks, and long-running tests. SwordFS does **not** treat that file as a
semantic skip list: the authoritative upstream-selected population is still
captured before exclusions, and a small separate `deferred-ci.tsv` records any
test intentionally not executed in the PR gate. Deferred tests remain visible
as selected-but-not-executed coverage debt, never count as supported, and must
not be relabeled `UPSTREAM_NOT_APPLICABLE` merely because ZeroFS excludes them.
After the initial rollout, deferred coverage is non-growing by default. A PR
that adds a new deferred testcase is blocked unless it carries the explicit
`conformance-deferred-change` review override; removing a deferred entry to
restore real execution never needs an override.

For the initial baseline we align only with the subset of ZeroFS exclusions
that are also in the pinned `generic/quick` population and are known to be
CI-prohibitive on FUSE: `generic/069`, `471`, and `478`. Their exact reasons are kept in
`conformance/fstests/deferred-ci.tsv`, and their restoration to bounded real
execution is tracked by GitHub Issue #248. ZeroFS currently excludes 38 generic
tests in its FUSE workflow, 33 of which are in this pinned quick population;
the other 30 quick exclusions are deliberately **not** inherited because they
encode ZeroFS/protocol capability decisions rather than PR-runtime constraints.
Protocol-specific exclusions from another client path are not inherited
without independent evidence.

This distinction is checked against SwordFS' current capability behavior, not
just ZeroFS' exclusion comments. For example, `generic/117` reaches
`_require_attrs` before its fsstress loop and SwordFS currently returns ENOSYS
for xattrs, so the test should produce a fast, useful NOTRUN result.
`generic/749` similarly probes `fallocate` before its mmap/SIGBUS body and
SwordFS currently returns ENOSYS for fallocate. `generic/504` is also kept in
the executed population so SwordFS' own flock behavior is observed instead of
inheriting another FUSE implementation's `/proc/locks` conclusion.

The peer check also exposed environment requirements that affect whether a
result is trustworthy. Upstream's Ubuntu package list is useful input, but it
cannot be copied mechanically onto a moving hosted-runner image. In the
September 2026 runner, the distribution `liburing-dev` package contains a
pre-generated compatibility header that expects a kernel header absent from
the image; installing the running kernel's header package in turn attempts a
large host-package upgrade and conflicts with the runner's preinstalled MPI
stack.

SwordFS therefore pins liburing alongside fstests and builds it from source
against the runner's actual userspace/kernel UAPI headers. This preserves
fstests' io_uring capability detection instead of manufacturing NOTRUN results,
without mutating the host kernel package set. The Ubuntu package step stays at
the smaller dependency set already proven installable on the hosted runner,
but includes selected-suite prerequisites once evidence shows their absence is
the only reason a testcase is skipped. In particular, `acct` and `fsverity`
are installed because the pinned `generic/quick` population otherwise records
tool-missing NOTRUN results rather than testing SwordFS behavior.
The pinned liburing include/library paths are injected explicitly into the
fstests configure, compile, link, and runtime environment because fstests'
liburing build rules use `-luring` after capability detection rather than
retaining the full `pkg-config` link path.
Environment-specific exclusions are therefore explicit coverage debt, not
evidence of filesystem support or non-applicability. They should move back
into the executed gate once a bounded/representative runner is available.

ZeroFS configures a finite filesystem quota specifically so fstests can reach
ENOSPC paths. SwordFS currently reports a fixed synthetic capacity from
`statfs` and does not decrement free blocks as data is written. Therefore an
ENOSPC/resource-pressure test that cannot reach its expected state is not
automatically `UPSTREAM_NOT_APPLICABLE`; it is a SwordFS capability/semantic
gap unless investigation proves a separate upstream assumption is incompatible
with the distributed FUSE model.

## FUSE and backend topology

fstests has a native generic FUSE mode. Its FUSE path treats `TEST_DEV` and
`SCRATCH_DEV` as mount-source identities and, when a test asks to recreate the
scratch filesystem, cleans the mounted scratch tree instead of assuming a
local block-device `mkfs` operation. They must still be usable by util-linux
as unmount targets because upstream calls `umount $TEST_DEV` / `umount
$SCRATCH_DEV` during testcase mount cycles.

SwordFS provides two independent fresh volumes to that interface:

```text
                         +-> TEST_DEV    -> SwordFS test volume
fstests -> mount.fuse.swordfs
                         +-> SCRATCH_DEV -> SwordFS scratch volume

each SwordFS volume:
  Linux VFS -> FUSE -> SwordFS -> Redis metadata
                                -> MinIO / S3-compatible data
```

`scripts/fstests-mount-helper.sh` is installed temporarily as
`/sbin/mount.fuse.swordfs` for the CI run. The runner uses each mountpoint
itself as the corresponding FUSE source identity (`TEST_DEV == TEST_DIR` and
`SCRATCH_DEV == SCRATCH_MNT`). `FSTYP=fuse` permits these opaque device values,
and using the mountpoint value avoids depending on util-linux to resolve an
arbitrary FUSE source path when upstream executes `umount $TEST_DEV` or
`umount $SCRATCH_DEV`. The helper maps the two identities to their SwordFS
volumes, preserves requested FUSE mount options, and sets `fsname` to that same
identity so fstests' source/target verification remains coherent. Earlier
attempts with both bare labels and separate absolute marker paths left stale
FUSE mounts; full CI artifacts showed those source values were not accepted as
unmount targets. `allow_other` is always enabled because upstream fstests
executes cases under its `fsgqa` users. SwordFS itself supplies its mandatory
`default_permissions` option.

The runner's temporary root is mode 0755 even though `mktemp -d` defaults to
0700. The directory remains root-owned, but execute permission is required so
upstream testcase processes running as `fsgqa` or other non-root identities can
traverse to TEST/SCRATCH. A fail-fast setup check executes `test -x` as
`fsgqa`; otherwise parent-directory permissions would manufacture `EACCES`
results before SwordFS authorization semantics are reached.

The authoritative fstests result never uses the in-memory metadata backend.
The runner starts Redis and MinIO through the same project-owned compose stack
used by other persistent-backend tests, formats two unique SwordFS volumes,
and mounts them through the normal SwordFS CLI and low-level FUSE path.
The artifact records both startup identity and teardown-time backend evidence:
container state/restart count/OOM status, Redis client/statistics and relevant
connection-limit/timeout configuration, plus Redis and MinIO logs. A backend
disconnect must therefore be classified from concrete service/runtime evidence
rather than inferred from a SwordFS-side EIO alone.

Before testcase selection or execution, the runner performs a fail-fast FUSE
mount lifecycle preflight using the same helper and TEST identity as fstests:
mount, verify `findmnt -S TEST_DEV` resolves to `TEST_DIR`, unmount by
`TEST_DEV`, verify the mountpoint disappears, and wait for the daemon PID to
exit before repeating an immediate remount/unmount cycle. The preflight records
source/target/type, daemon PID, and `/proc/self/mountinfo` evidence. This
distinguishes kernel unmount completion from userspace-daemon teardown and
prevents an invalid FUSE identity or teardown race from contaminating hundreds
of testcase results before the harness notices.

The dry-run selection remains one canonical upstream `generic/quick` run so the
645-case denominator cannot be curated by the SwordFS harness. Real execution
then invokes the **unmodified pinned upstream `check` once per selected,
non-deferred testcase**. Each invocation still performs upstream fstests'
normal init, testcase body, result qualification, XUnit generation, and
TEST/SCRATCH cleanup. The outer runner waits for both SwordFS daemon PIDs and
mountpoints to disappear before starting the next testcase. If cleanup needs
runner intervention, that testcase receives `.isolationfail` evidence and is
classified as infrastructure; the recovered environment can then continue
collecting independent evidence for later tests instead of propagating a stale
userspace FUSE client through the rest of the population.

Upstream fstests' own `.mountfail` file has a different meaning. `check`
retains it only as diagnostic detail for a testcase that already failed, and
some tests intentionally probe mount options that may fail. An upstream
`.mountfail` therefore remains ordinary testcase FAIL evidence and can be
reviewed as an exact known semantic/unsupported gap; it does not by itself mean
that the SwordFS wrapper lost isolation.

This isolation means a supported testcase is a claim about that testcase's
filesystem semantics on a clean SwordFS FUSE session over the shared persistent
backend. It is deliberately **not** a claim that a long-lived client can recover
after an earlier testcase has already driven that client into a transport or
backend failure state. Such recovery/stability defects are tracked independently
rather than being allowed to contaminate or silently redefine unrelated
conformance results.

Per-test XUnit files are preserved under `raw/xunit-parts/` and merged without
rewriting testcase outcomes into the single `raw/results/result.xml` consumed
by the classifier. The overall suite deadline remains the pinned `FSTESTS_SUITE_TIMEOUT` (75m);
an individual test may consume the remaining deadline rather than being
subjected to an arbitrary short per-test timeout.

The runner also records Redis `total_connections_received` after each testcase
in `raw/redis-connections.tsv`. The delta is an interval-level diagnostic, not
an attribution counter: it includes the testcase workload plus expected harness
traffic such as the testcase's fresh SwordFS client/pool setup, Redis' periodic
Docker healthcheck, the sampling `redis-cli INFO` connection, and an additional
backend-health probe after a failing testcase. It is therefore used to locate
abnormal connection-churn spikes relative to neighboring testcases, not as an
exact reconnect count. This evidence is diagnostic only and is not a pass/fail
criterion.

The testcase list is read through a dedicated shell file descriptor and each
upstream `check` invocation receives `/dev/null` as standard input, so a
test/helper cannot consume the remaining execution list. After the loop, the
runner requires the number of recorded testcase statuses to equal the planned
non-deferred population unless execution stopped for an explicit timeout or
infrastructure failure.

## Baseline model

The baseline has two exact testcase-level inputs:

- `conformance/fstests/supported.txt` lists behavior SwordFS claims to support;
- `conformance/fstests/known-gaps.tsv` records evidence-backed known outcomes.

Wildcard selectors and directory-level exclusions are not supported. A
semantic defect or missing SwordFS capability must reference a focused GitHub
Issue. Known-gap categories are:

- `known_semantic_defect` — implemented behavior violates the tested semantic
  contract;
- `known_unsupported` — the testcase requires a SwordFS capability that is not
  implemented yet;
- `upstream_not_applicable` — the testcase fundamentally cannot apply to the
  SwordFS FUSE/distributed model for a concrete upstream capability reason;
- `environment` — a stable limitation of the supported CI environment rather
  than a SwordFS semantic result.

### Current admitted baseline

The initial non-empty baseline was admitted from authoritative crash-free run
`35699074109` on PR #244 head `b3c90be`. The pinned `generic/quick`
selection contains 645 testcases: 642 are executed in PR CI and three remain
explicit deferred coverage debt under #248 (`generic/069`, `generic/471`, and
`generic/478`).

The admitted baseline contains 101 exact supported testcase IDs and 541 exact
known-gap outcomes. Those gaps are split by evidence rather than by raw
PASS/FAIL/NOTRUN result alone: 383 are `upstream_not_applicable`, 141 are
explicit SwordFS `known_unsupported` capabilities tracked by #255, 10 are
`known_semantic_defect` outcomes tracked by #256, and 7 are stable hosted-runner
`environment` limitations. The strict classifier was run against the same raw
selection/result evidence before admission and produced zero blocking outcomes.

This baseline is a reviewed snapshot of the current pinned environment, not a
blanket acceptance of upstream skips. Future XPASS, reason changes, supported
regressions, population shrinkage, daemon crashes, or wrapper isolation
failures remain blocking under the rules below.

`UPSTREAM_NOT_APPLICABLE` is deliberately strict. A testcase becoming
`notrun` is not enough to receive that classification. Every expected NOTRUN
entry records the exact normalized upstream skip message. If SwordFS merely
lacks the feature that caused the skip, the correct classification is
`known_unsupported` with a linked Issue, not `upstream_not_applicable`.
Any known-gap entry that expects a raw FAIL must preserve the exact normalized
XUnit failure message, regardless of category. This binds the baseline to the
reviewed XUnit failure class/message rather than merely to the testcase ID.
When fstests reports only a generic `output mismatch` message, the detailed
`.out.bad` evidence remains part of the CI artifact and must be reviewed when
establishing or changing that baseline entry. Applicability and environment
entries may likewise preserve a reviewed raw FAIL when the upstream testcase
does not convert that precondition into NOTRUN. A changed XUnit failure
message blocks as `BASELINE_REASON_MISMATCH` for every FAIL baseline.

Message normalization removes whitespace-only variation and canonicalizes only
the runner-owned random `/tmp/swordfs-fstests.<suffix>` work-directory prefix
to `<FSTESTS_WORK_DIR>`. Upstream reasons may embed TEST/SCRATCH paths (for
example a FITRIM prerequisite), and the mktemp suffix is execution noise rather
than filesystem semantics. The path below that root and the rest of the reason
remain exact baseline data. Upstream FAIL messages also embed the absolute
`build/fstests-conformance/raw/results` artifact root; that runner/workspace
prefix is normalized to `<FSTESTS_RESULT_DIR>` while retaining the exact
testcase-specific `.out.bad` / `.mountfail` suffix and failure class.

The classifier applies this state model:

```text
supported + PASS       -> PASS
supported + FAIL       -> REGRESSION                 (blocking)
supported + NOTRUN     -> BASELINE_NOT_APPLICABLE    (blocking)

known gap + expected observed result -> KNOWN_* / ENVIRONMENT
known gap + PASS                    -> XPASS          (blocking)
known gap + different FAIL/NOTRUN   -> BASELINE_RESULT_MISMATCH
known NOTRUN + changed skip reason  -> BASELINE_REASON_MISMATCH
known FAIL + changed failure reason -> BASELINE_REASON_MISMATCH

unclassified + PASS    -> UNCLASSIFIED_PASS          (blocking baseline debt)
unclassified + FAIL    -> UNEXPECTED_FAIL            (blocking)
unclassified + NOTRUN  -> UNCLASSIFIED_NOTRUN        (blocking)

selected testcase missing from result -> INFRASTRUCTURE (blocking)
unexpected testcase in result          -> INFRASTRUCTURE (blocking)
baseline testcase no longer selected   -> BASELINE_NOT_SELECTED (blocking)
SwordFS daemon core-dump evidence           -> SWORD_FS_CRASH (blocking)
runner `.isolationfail` intervention evidence -> INFRASTRUCTURE (blocking)
```

SwordFS daemon crashes are never admissible baseline outcomes. The classifier
inspects testcase `.full` evidence and, when it explicitly records the
`swordfs` process dumping core, forces `SWORD_FS_CRASH` before mount-failure,
known-gap, PASS/FAIL, or NOTRUN matching. This preserves the production crash
even if upstream subsequently reports a skip or cannot remount the filesystem.
Core dumps from fstests helper programs such as `xfs_io` do not trigger this
classification.

`*.isolationfail` is never an admissible semantic/unsupported known gap. It
means the SwordFS wrapper had to repair testcase cleanup and therefore cannot
trust isolation for subsequent evidence. The classifier checks the raw result
directory and forces such cases to blocking `INFRASTRUCTURE`, even if the
testcase also has a known-gap entry. Upstream `*.mountfail` remains raw fstests
diagnostic evidence and does not override the testcase's XUnit classification.

The first CI run for a new upstream population is discovery by design. The
classifier emits `bootstrap-supported.txt` from unclassified passes and a
`bootstrap-gaps.tsv` candidate list for failures/notruns. Those files are
investigation aids, not an automatically accepted baseline. Failures must be
grouped by root cause, focused Issues must be created for real SwordFS gaps,
and NOTRUN applicability must be validated before the baseline is committed.

## Regression invariants

Once the baseline is established:

- every supported testcase must remain selected and pass;
- supported coverage is monotonic by default;
- removing supported coverage requires the explicitly reviewed
  `conformance-semantic-change` PR label;
- a known gap that starts passing is XPASS and blocks until it is moved into
  `supported.txt`;
- a known NOTRUN whose reason changes blocks until the classification is
  re-evaluated;
- a testcase that disappears from the selected or observed population is a
  baseline/infrastructure error, never an implicit success;
- an upstream pin or selector change is a normal source change that must be
  reviewed together with its resulting baseline delta.

The supported set is therefore a claim about already-working semantics, not a
filter over the upstream suite.

## Execution, time bounds, and evidence

`scripts/run-fstests.sh` owns the privileged CI execution environment. It:

1. fetches and verifies the exact pinned liburing and fstests commits;
2. builds pinned liburing against the runner headers and builds the upstream
   fstests helper programs against that pinned library;
3. creates fstests' standard QA users and matching groups when absent,
   including `fsgqa2` and the digit-prefixed `123456-fsgqa` identities used by
   generic permission/quota tests, and verifies those users can traverse the
   root-owned CI work directory;
4. starts Redis and MinIO and formats fresh TEST and SCRATCH SwordFS volumes;
5. installs the temporary FUSE mount helper and writes fstests `local.config`;
6. records the exact upstream-selected testcase population in XUnit;
7. runs the selected population minus the exact entries in
   `conformance/fstests/deferred-ci.tsv` as isolated, exact single-test
   invocations of the unmodified upstream `check`, under one bounded overall
   suite deadline. The full pre-exclusion selection remains the authoritative
   denominator;
8. preserves raw fstests output, per-test and merged XUnit, environment metadata, SwordFS logs,
   concrete Redis/MinIO image identities and versions, final backend container
   state, Redis INFO, Redis/MinIO runtime logs, mount/isolation evidence, and
   `dmesg` when the runner permits reading it; and
9. tears down FUSE mounts and backend services on every exit path.

The suite's raw exit code is not the semantic gate because fstests normally
returns non-zero for any testcase failure, including a known gap. The strict
classifier interprets individual results against the checked-in baseline.
Harness timeout, missing/malformed XUnit, population mismatch, or other runner
failure is classified as `INFRASTRUCTURE` and blocks independently of known
semantic debt. The only exception to "selected testcase missing from result"
is an exact entry in `deferred-ci.tsv`; the classifier renders it as
`DEFERRED_CI`, reports it separately, and rejects overlap with `supported.txt`
or `known-gaps.tsv`. A deferred testcase that disappears from the upstream
selection is also blocking, so the list cannot silently become stale.

The initial deferral exists because `generic/quick` contains 645 tests at the
pinned revision and "quick" is calibrated for conventional local filesystems,
not necessarily FUSE. ZeroFS documents `generic/069` at roughly 16 minutes on
its FUSE path and identifies `generic/471` as long-running and `generic/478`
as capable of hanging in its locking path. A blanket
three-minute per-test timeout would still add tens of minutes when several such
tests hit the bound and would change every testcase's execution environment.
Explicit temporary deferral is therefore simpler and more honest for the PR
gate. These tests should later be restored through a dedicated slow/FUSE job or
after their applicability/runtime behavior is independently characterized.

Discovery also exposed a separate SwordFS stability defect: the
`generic/006` directory-entry workload can leave an existing Redis-backed
client reporting repeated connection resets and EIO while newly created
clients can connect to the same backend. That production root cause is tracked
by Issue #249 without assuming Redis itself or the Redis client is the root
cause. It is neither an environment skip nor a reason to defer the testcase.

fstests is environment-sensitive and privileged, so complete execution belongs
in GitHub Actions. Local development verifies the runner/classifier syntax and
classifier unit tests; GitHub CI is authoritative for the mounted FUSE run.

### Stable progress publication

Every authoritative `main` run publishes the classifier output to the stable
`fstests-status` branch. The branch contains `status.md`, `latest.json`, and
`history.json`: the Markdown page is the durable human-facing compatibility
report linked from README, while the JSON files preserve the latest machine-
readable result and bounded publication history. Raw execution outcomes
(`PASS`, `FAIL`, `NOTRUN`, and `DEFERRED`) are reported alongside semantic
classifications so a reader can distinguish observed test outcomes from the
reason a non-PASS result is currently accepted.

The status branch is refreshed after every `main` fstests job, including an
infrastructure-failed run. An infrastructure failure must replace a stale
healthy headline rather than silently leaving the previous run looking current.
`history.json` records each authoritative publication; the rendered historical
progress table adds a row only when meaningful counts change, so unchanged main
commits do not create visual noise. Because fstests runs are long enough for two
`main` workflows to finish out of order, publication records the GitHub run id
and refuses to replace a status already produced by a newer run. A concurrent
status-branch push is retried after refetching the latest branch state. Raw CI
artifacts remain the authoritative detailed evidence for each run.

Publication is intentionally isolated in `publish-fstests-status`, which needs
`contents: write` only on `main`. It is not a pre-merge required check; the
required semantic gate remains `fstests-conformance (Release)`.

The first rollout has an ordering constraint for repository protection. PRs
introducing a brand-new status-check context cannot safely make that context a
repository-wide required check before the workflow exists on `main`, because
unrelated PRs would then wait for a context their target branch cannot emit.
Therefore #237 must itself treat `fstests-conformance (Release)` as a manual
merge gate, merge only after that job is green, and then immediately add the
context to the live `CI-Must-Pass` ruleset and verify it. Subsequent PRs are
protected by the normal required-check mechanism.

## Relationship to Issue tracking

The baseline is intentionally test-specific but Issue grouping is
root-cause-specific. One validated missing capability or semantic defect may
own many exact fstests testcase rows when the evidence demonstrates the same
cause. Conversely, a still-failing testcase must be re-attributed when later
evidence shows that its former root cause no longer explains the failure.

This keeps the regression contract auditable without creating one Issue per
upstream testcase or hiding independent defects behind a broad exclusion.
