# CI critical-path and runner-cost optimization (#462)

## Goals and invariants

For non-documentation PRs, reduce critical-path latency and total hosted runner
minutes **without** changing filesystem semantics, conformance case population,
per-case fstests mount/process isolation, test classification, branch-protection
contexts, or code coverage. P50 <=12m and P95 <=15m are *targets*, not assertions.
Cold-start runner and network delays require separately measured distributions.

## Evidence and runtime planning

The checked-in `conformance/fstests/runtime.tsv` previously used one historical
run (35740846851). Three independently successful full runs on 2026-10-12
provided complete XUnit timings for all **142 supported cases**:

| GitHub Actions run | Observed supported cases | generic/531 | generic/247 |
| --- | ---: | ---: | ---: |
| [38227069396](https://github.com/SwordInfra/SwordFS/actions/runs/38227069396) | 142 | 524s | 196s |
| [38230206097](https://github.com/SwordInfra/SwordFS/actions/runs/38230206097) | 142 | 536s | 197s |
| [38232039933](https://github.com/SwordInfra/SwordFS/actions/runs/38232039933) | 142 | 526s | 200s |

For each supported case use the median of the three measured XUnit
`testcase.time` values; the median rejects a single anomalous sample without
pretending an aborted/timeout run produced a completed measurement. Retain
historical weights only for the three bounded cases not observed in these
supported shards. This change **only affects deterministic test placement**:
the selected/deferred/blocked source files, stable nine shard names,
per-case test invocations, and authoritative classification are untouched.

Historical weights spread the four supported shards at ~396s *predicted*
each, although one actual run consumed approximately 802/386/284/535s of
reported testcase time. The refreshed planner predicts 526/510/509/509s.
`generic/531` occupies `supported-0` by itself. These are modeled
testcase totals, not measured new CI durations, and exclude harness overhead,
artifact delivery, environment setup, and queue time.

## Release core distribution

1. `change-scope` decides whether a full CI run is necessary. The
   `build-release-core` job starts immediately afterward, **without** waiting
   for clang-format/MinIO setup, on pinned `ubuntu-24.04`. It builds only
   the uninstrumented `Release` `swordfs` target.
2. Its Python packager uses `ldd` to discover ELF shared-library closure,
   copies the runtime libraries beside the binary, installs `$ORIGIN`-relative
   RUNPATHs with `patchelf`, and checks the resulting runtime via `ldd -r`
   and `--help`. The glibc dynamic loader/libc and related glibc libraries
   remain supplied by the *same pinned runner image* for producer and consumers.
   Neither intermediate CMake files nor a whole native build cache is shipped.
3. A private per-run v4 artifact is published. Its trusted producer job output
   records the SHA-256 of the archive. The manifest also records PR-head/main
   commit SHA, run ID, attempt, target build configuration, architecture,
   runner image, exact file inventory and file hashes. Ten conformance consumers
   require the producer's successful status, download the same archive, check
   the producer hash and every manifest field/hash, then smoke-test the
   extracted ELF closure **before** copying it into `build/swordfs`.
   Missing/wrong artifacts, mismatched commits/attempts, damaged data,
   unresolved libraries, or non-executable binaries fail the consumer job.
4. Consumers keep pinned harness utilities, service dependencies, FUSE
   permissions, and runtime/test-harness packages, **not** SwordFS native
   toolchain/development setup or SwordFS CMake/build invocations.

### Reproducing the runtime baseline

The first implementation correctly shared the ELF and all 645 cases executed,
but run [38259564358](https://github.com/SwordInfra/SwordFS/actions/runs/38259564358)
failed the strict aggregate classifier for two *infrastructure* reasons:

1. Both `baseline-rest` shards independently generated an empty
   `generic/check_dmesg` sentinel. The aggregate collector now accepts
   **only** byte-empty copies of this precise upstream auxiliary filename;
   all nonempty/other duplicate evidence still fails closed. On the archived
   failed run, re-aggregation reconstructs 645 unique testcase results with
   no infrastructure errors.
2. The old `install-deps.sh` step indirectly upgraded the test VM from
   Ubuntu 24.04 defaults to the resolute packages. The historical passing
   baseline used `fusermount3 3.18.2`, `xfsprogs 6.18.0-3` and libc
   2.43. Without the native installer, the new consumer used
   `fusermount3 3.14.0`, `xfsprogs 6.6.0` and libc 2.39. The older
   `xfs_io` changed upstream skip/probe diagnostics (`exchangerange`,
   `startupdate`, `fwzero`, `cachestat`, and atomic-write capability);
   those changed reasons violated the exact known-gap contracts. This was
   **not** a supported SwordFS regression: 144 supported tests still passed.
   `install-conformance-runtime.sh` now explicitly installs the previously
   used `fuse3=3.18.2-1`, `libfuse3-4=3.18.2-1`, and for fstests,
   `xfsprogs=6.18.0-3`, from the previously used resolute repository.
   The script verifies exact package versions and gives each APT operation
   a finite 240s deadline. Other conformance runtime dependencies remain
   separate from the native *build* dependency installer.

Do not mutate known-gap expected reasons to accommodate a changed tool
version. When a pinned dependency cannot be installed, the job must fail,
not silently switch to a different probe/classification environment.

The independent Release UT job remains parallel and retains its required
`build-and-test (Release)` context. Combining its longer `swordfs_test`
build with the shared producer would serialize all conformance cases behind
the extra unit-test compilation. Debug UT plus coverage, clang-tidy/dead-code
audit, and Release E2E plus coverage retain their independent build variants.
No new required check replaces an existing context. Full-run consumers depend
on a successful producer and explicitly fail closed if any prerequisite failed;
the existing `fstests-conformance (Release)` aggregate remains authoritative.
Documentation-only PRs skip this producer along with the existing full suite.

### Shared pinned fstests/liburing toolchain (second optimization)

Run [38263341993](https://github.com/SwordInfra/SwordFS/actions/runs/38263341993)
finished **GREEN** in **23m32s** (the historical P50 was 21m26s), although
runner consumption fell to **125.78** from **137.24 runner-minutes** (8.4%).
Its last `bounded-478` consumer spent 6m27s waiting after the Release
producer completed, then ran 12m40s. On the same run, the fstests harness
repeatedly fetched and built *identical* pinned liburing and xfstests trees
for every shard. The `bounded-478` log shows approximately 178s before
first testcase (including liburing/fstests compilation, account creation,
services, mount preflight); `supported-3` approximately 86s.

The `build-fstests-tools` producer now starts directly after
`change-scope`, **in parallel** with `build-release-core` and without
waiting for MinIO or clang-format. It installs the exact pinned conformance
runtime, builds liburing and xfstests one time, applies the same reviewed
`generic/615` reliability patch and builds at the fixed path
`/opt/swordfs-fstests-tools`. A fixed prefix is necessary because upstream
`vfstest` executes under setuid and carries an absolute liburing RPATH;
changing the installed location after compilation would break it.

The producer uploads a same-run compressed tree (upstream working sources
and executables plus installed liburing, but **no git histories or liburing
compiler source**). Nine fstests consumers require a successful producer,
verify the producer-provided archive SHA-256, source commit/run/attempt,
runner image/arch/glibc, exact upstream commits, and applied patch hash
before installing at the **same fixed absolute path**. A missing or corrupt
artifact or mismatched ABI/pins fails the shard; there is no silent CI
fallback to per-shard compilation. Local harness execution can still compile
the exact versions in its own isolated temporary workspace.

The test runtime remains independent: each shard has its own `WORK_DIR`,
`TEST_DIR`, `SCRATCH_MNT`, Redis/MinIO backend, fresh volumes, mount
setup, per-test `./check` process and timeout/isolation behavior. The
selected 645-case population, stable nine shards, XUnit evidence, known-gap
baseline and required `fstests-conformance (Release)` context are unchanged.
This is explicitly an experiment in wall-clock reduction: the extra
producer can introduce its own wait barrier, so measure actual GitHub CI
success cases rather than assuming a speedup from removing duplicate work.

The first full run with shared tools ([38268808406](https://github.com/SwordInfra/SwordFS/actions/runs/38268808406))
completed all nine shards successfully but the aggregate initially flagged
two known-gap **path-only** mismatches: `generic/184` embedded the new
absolute upstream test script path in a failure report, and `generic/780`
embedded the `src/file_attr` helper path in its NOTRUN reason. The classifier
now canonicalizes **only** the fixed `/opt/swordfs-fstests-tools/` prefix to
the same harness-owned path placeholder used for the prior temporary upstream
checkout. It retains the full relative path, result, failure details,
testcase membership, and existing checked-in known-gap expectations.
Reclassification against the actual downloaded failed-run aggregate yields
645/645 executed, 144/144 supported passing, and **zero** blocking outcomes.
No expected failures, testcase selectors, or semantic classifier gates were
weakened.

### Six-way supported-case parallelism (third optimization)

The second implementation's full-green
[run 38271327618](https://github.com/SwordInfra/SwordFS/actions/runs/38271327618)
finished in **19m47s** using **116.83 runner-minutes**, versus the 12-run
historical full-CI P50 of **21m26s** and **137.24 runner-minutes**.
Its longest job was `supported-2`: **15m45s**, including **13m52s** in the
pinned fstests harness. Full-suite P50 <=12m/P95 <=15m remains unmet.

Deterministic offline replanning with the same version, 645-test selection,
144-case supported manifest, three explicit bounded cases, and the recorded
per-case weights gives the following supported-shard case-time predictions:

| Supported shard count | Largest supported shard | Remaining supported shards |
| ---: | ---: | ---: |
| 4 (second optimization) | 526s, 1 case | 509–510s, 46–48 cases |
| **6 (chosen)** | **526s, 1 case** | **305–306s, 26–29 cases** |
| 8 (considered, not chosen) | 526s, 1 case | 216–230s, 1–25 cases |

The 526-second floor is `generic/531`, which is already alone on
`supported-0`. Six supported shards therefore reduce expected work on
the other long supported shards by approximately 40% without creating
two more runners whose work cannot shorten the single-test floor. This
is a *model prediction*, not a wall-time improvement claim: per-shard
setup (pinned packages, verified artifacts, FUSE/service initialization),
runner scheduling, long-test variability, and XUnit overhead remain.

The CI matrix now has **six supported**, **two baseline-rest**, and
**three bounded** shards (11 total). The same deterministic load-balancing
algorithm and case-level isolation remain intact, the bounded 069/471/478
testcases remain in their designated dedicated jobs, and aggregation
derives the complete expected shard set directly from the planner.
Fail-closed shard artifacts, 645/645 executed case coverage, 100% supported
gate, known-gap classification, all required checks, and full runner-minute
accounting are unchanged. An actual complete GitHub CI run must establish
whether the increased parallelism improves end-to-end wall time enough
to offset the extra runner startups.

### Single-transaction conformance runtime provisioning (fourth optimization)

The six-way successful [run 38343259419](https://github.com/SwordInfra/SwordFS/actions/runs/38343259419)
finished in **15m25s** and **117.75 runner-minutes**, but each of eleven
fstests consumers separately performed three APT transactions: the pinned
FUSE/xfsprogs installation, a second install of the *producer's* xfstests
build prerequisites, and gdb/strace diagnostics. On its critical-path
`supported-0` job these steps took **30s + 79s + 7s**, respectively,
before the fstests harness. Several of the unpinned development packages
also pulled unrelated release upgrades and system service restarts.

The shared fstests build artifact already contains compiled upstream
helpers and liburing at a verified fixed RPATH. Its **build producer** now
installs all required development packages in one bounded APT transaction
(`fstests-build` mode); **consumers** install runtime tools, shared-library
runtime dependencies, and gdb/strace in one transaction (`fstests` mode)
without installing autoconf/automake, libtool, header/-dev packages, or
the xfstests compilation toolchain. The pjdfstest job similarly installs
its required build tools in one transaction (`pjdfstest` mode).

The resolute APT source, exact `fuse3=3.18.2-1`,
`libfuse3-4=3.18.2-1`, and `xfsprogs=6.18.0-3` versions, strict
post-install version assertions, 240-second APT timeout, and complete
runtime testcase/execution contracts are retained. The installation uses
`--no-install-recommends` to avoid unrelated package upgrades; explicit
shared-library packages cover the binaries produced upstream. Diagnostics
remain preinstalled, so a timeout never depends on working APT/network
access to capture its evidence. The changes target *package installation
and runner-minute cost*, not fstests test selection, severity, or semantics.

The expected reduction is not a verified speedup until a complete green
CI run proves runtime dependency closure, all 645 testcase results and
known-gap reasons, as well as the real runner and wall-clock times.

### Narrow Boost package dependency closure (fifth experiment)

The native dependency installer runs separately on the Release runtime
producer, Debug/Release unit-test builds, E2E coverage build, and the static
audit. On the fully green fourth-round run 38356195892, the Release producer
spent **132s of its 223s** job provisioning build dependencies. Its raw APT
log shows `libboost-all-dev` bringing in components unrelated to SwordFS,
including MPI and RDMA build libraries. This potentially delays *every*
fstests consumer because the producer's verified binary is a prerequisite.

The pinned upstream Folly `v2026.07.20.00` installed
`CMake/folly-config.cmake.in` requires precisely the five Boost CMake
components `context`, `filesystem`, `program_options`, `regex`, and
`thread`. The upstream `CMake/folly-deps.cmake` requires a subset of these
when building Folly. SwordFS's own CMake directly links the Folly target;
it does not require additional Boost components. Therefore replace
`libboost-all-dev` with those five development package names; their
normal system-package dependencies provide the necessary Boost headers and
base libraries. This changes neither the native dependency source/version
cache inputs nor the compiled project targets and tests.

Hypothesis: fewer transitive packages to install will reduce cold Release
producer APT time and total build runner-minutes. This is *not* validated
until CI proves the pinned cached native dependencies remain linkable,
the cold-system-package installation satisfies Folly's imported CMake
targets, all builds and static audit pass, and the complete 645-case
conformance result is unchanged. Report the GitHub CI measurements and
whether the long-tail critical path actually shortens.

## Dependency tail and observability

The same native-dependency installer is also used by manually dispatched
`dev-build.yml`. Its cached library inputs cannot protect against slow APT
index refreshes: run 38263576623 observed a >32-minute Dev Build stall.
The Dev Build caller therefore explicitly passes
`SWORDFS_CI_APT_TIMEOUT_SECONDS=180` to bound each APT operation, just as
the required full CI callers use a 240-second bound. This makes a stalled
mirror fail diagnosably rather than silently consuming most of the focused
development job deadline. Neither route retries failed package installs.

The anomalous 18m22s `Install dependencies` step was measured in the
`supported-2` job of [run 38227069396](https://github.com/SwordInfra/SwordFS/actions/runs/38227069396).
Its timestamped raw job log shows a system-package cache miss at **09:02:36**
and the *first* APT package output at **09:19:43**: an opaque **1,027-second
gap** before native Folly/AWS dependency verification even began. The old
log cannot distinguish APT index refresh from install startup/download
latency, so an exact mirror/network root cause is not yet established.
Separating all ten conformance consumers from that installer removes this
source of variation from their critical path. Remaining build jobs log
distinct APT index and installation phases and native dependency cache hits.
In formal CI, each APT invocation is also bounded to 240 seconds with
connection timeouts: unusually slow provisioning now fails diagnostically
instead of silently occupying a runner for 18 minutes. These guards do not
retry or hide deterministic failures. A reproducible preinstalled runner
image remains a possible follow-up if measured producer waits dominate.

Performance review must compare similar complete successful code PR runs,
report elapsed wall time, aggregate runner time, producer wait, runtime
transfer and the longest shard. Three historical runs establish an initial
fstests timing sample; for overall wall time and runner cost, 12 recent
successful non-docs-only PR CI runs (2026-10-11 through 2026-10-12) provide
a separate baseline. Every sampled workflow contains 21 jobs: run IDs
38252951381, 38251191212, 38232039933, 38230206097, 38227069396,
38201710698, 38199801921, 38192272185, 38147638975, 38127555991,
38127369899, and 38119500346. Their workflow elapsed-time P50 is
**21.44m**, P95 **34.01m**; median aggregate completed-job runtime is
**137.24 runner-minutes**. These include both normal runs and long tails;
they do not constitute a post-change performance claim.

The CI timing summary reports per-job and per-step durations, job queue
delay, artifact size, upload/download steps, producer-to-consumer wait and
XUnit testcase time. It also compares the 12 most recent successful
full-length PR CI runs and prints an **informational sustained regression
warning** only when the latest five median times exceed the preceding five
by both >20% and >3 minutes. This is deliberately not a hard CI failure
threshold; runner and network variance must not create flaky merge gates.

## Verification contract

- Check YAML/pre-commit, runtime-file sample provenance, sorted case inventory
  for old/new plans (645 unchanged, exact one-time case assignment), and
  aggregate classifier regression fixtures without local SwordFS compilation.
- Exercise the packer/verifier on a small dynamic ELF fixture (not SwordFS)
  including damage, manifest/hash/commit/permission mismatch and missing
  shared-library failure modes.
- Exercise full CI through PR, main push and workflow_dispatch; require all
  existing branch-protection status contexts. The authoritative first
  build/link/runtime/FUSE integration verification is GitHub Actions, not a
  local development-server compile.
- Compare complete CI timings *after* successful runs, distinguishing
  predicted balancing from observed speedup.
