# Redis Metadata Test Organization

Issue: #352

## Goal

Keep the Redis metadata regression suite as a long-lived specification instead of an append-only record of individual fixes. This is a test-only refactor: production behavior, Redis schema, metadata semantics, and backend-parity coverage remain unchanged.

## Organization

The Redis metadata tests are organized by the contract a maintainer is looking for:

- `RedisMetaClientTest.cpp` owns Redis client connection, transaction-boundary, retry, and direct client behavior.
- `RedisMetaClientFaultServer.hpp` owns the scripted RESP fault server used by Redis client failure-boundary tests, keeping protocol fault injection out of the contract test body.
- `RedisBackendContextTest.cpp` owns backend-context lifetime and shutdown semantics.
- `RedisMetaOpsTest.cpp` owns the `RedisMetaOps` read/transaction facade.
- `RedisMetaTxnNamespaceTest.cpp` owns namespace and inode-state transaction primitives.
- `RedisMetaTxnChunkTest.cpp` owns chunk publication, truncation, private-index, and pending-delete transaction semantics.
- `RedisMetaTxnSetAttrTest.cpp` owns SetAttr/killpriv transaction semantics.
- `RedisMetaTxnReclaimTest.cpp` owns reclaim serialization and optimistic-WATCH concurrency semantics.
- `RedisMetaImplRenameTest.cpp` owns Redis-backed rename and `RENAME_EXCHANGE` behavior.
- `RedisMetaImplReclaimTest.cpp` owns Redis-backed orphan/reclaim lifecycle and reclaim visitor behavior.
- `RedisMetaImplTest.cpp` retains the remaining broad integration-style Redis metadata contracts that are not yet large enough to justify another domain file.

Shared test-only headers may hold fixture/setup mechanics that are common to several translation units. They must not expose production-only seams or hide the semantic steps of an individual test.

## Invariants

1. **Backend parity is preserved.** Equivalent Mem and Redis semantic tests are independent coverage because the backends have independent implementations.
2. **No production test seam is introduced.** Redis state needed for corruption/concurrency setup is reached through existing production configuration or direct Redis test setup.
3. **Completion barriers remain semantic.** Fiber/concurrency assertions run only after the real operation completion barrier; moving a test must not replace it with an intermediate notification.
4. **Mechanism-level concurrency assertions are intentional.** The #296 retry-count checks protect #280's WATCH-scope design:
   - unrelated `reclaims` Hash entries must not invalidate a transaction (`attempts == 1`);
   - a same-inode Link commit must invalidate the reclaim snapshot and force one retry (`attempts == 2`).
   These assertions stay close to explanatory comments so they are not mistaken for incidental implementation coupling.
5. **Observable outcomes are preferred elsewhere.** White-box Redis keys/layout/call sequence should only remain when they independently protect a correctness or performance invariant.

## Fixture strategy

- Common Redis URL parsing, connection options, unique namespaces, inode/entry seeding, and raw-state helpers are test-only shared helpers rather than copy/paste across files.
- Client-specific RESP fault injection remains in a client-only test helper instead of bloating every Redis metadata test translation unit.
- The `RedisMetaImpl` fixture is shared by the general, rename, and reclaim translation units so volume initialization and raw Redis access have one definition.
- Helpers should make setup shorter without abstracting away the scenario under test; domain-specific setup stays local to its domain file.

## Verification

This task does not use RED -> GREEN because it is a pure test refactor with no production behavior change.

Verification is:

1. structural inventory: all pre-refactor test names remain present exactly once unless intentionally renamed for a stable contract;
2. formatting/static repository checks (`pre-commit`, `git diff --check`);
3. GitHub `dev-build.yml` focused build/unit-test run for Redis metadata suites;
4. full required pull-request CI after the Draft PR is opened;
5. final review for retained #296/#330/#340 regressions, readable domain ownership, no test-only production API, and no weakened assertions.

No local SwordFS compilation is required for this refactor; GitHub development CI is the focused compile/test environment.
