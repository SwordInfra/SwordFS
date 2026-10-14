# Request-scoped cancellation and FiberRuntime event dispatch (#477)

## Contract and ownership

Each asynchronous FUSE request has exactly one `folly::CancellationSource`, owned
by its FUSE request adapter until the request has completed its reply. The
request's fiber-local `utils::SwordFsContext` contains only a *copy* of the
source's `folly::CancellationToken`. Downstream VFS, metadata and IO code may
observe cancellation or register `folly::CancellationCallback`, but cannot
cancel a request through its context.

The libfuse interrupt hook only calls `CancellationSource::requestCancellation()`.
It never replies, mutates Redis or locks, nor directly touches a Folly fiber.
The hook may fire synchronously during registration if the interrupt arrived
first, or concurrently on a libfuse worker. Every FUSE completion unregisters
the hook **before** calling a consuming `fuse_reply_*` function, including on
failure and rejection. Unregistration synchronizes with an in-flight libfuse
callback; the source remains alive through its registration and reply handoff.
The request owner makes exactly one reply; cancellation alone never implies
`EINTR`, rollback, or re-reply.

The request admission owner also retains the rejection callback and a
once-only rejection flag. This state is prepared before interrupt registration.
If fiber runtime initialization, capture transfer or task admission throws
after registration, the FUSE adapter catches the exception and uses the same
rejection path as normal runtime rejection. The reply adapter first unregisters
the interrupt handler (waiting for an in-flight callback) while the shared
cancellation source remains alive, then consumes the request. Once-only
rejection prevents an exceptional handoff from issuing a duplicate reply.
The exceptional runtime-initialization allocation/thread failure is protected
structurally: the ordinary test harness does not provide deterministic
resource-allocation or thread-creation failure injection without affecting
unrelated production behavior.

`SwordFsContext` retains its token when caller identity is populated from the
FUSE request; ordinary non-FUSE work receives a default non-cancelled token.

## FiberRuntime event notification

`FiberRuntime::Post(folly::Function<void()>)` queues a callback on the owning
EventBase driver **without creating a business Fiber**. Like `Submit`, `Post`
is rejected once shutdown starts; admitted callbacks participate in the
runtime's drain barrier. The callback executes on the owning driver thread,
but outside a Folly fiber; it may post a thread-safe `utils::FiberBaton`, not
assume it is inside a fiber execution domain. Operations register their own
`folly::CancellationCallback` and arrange domain-safe wakeups via `Post`.
`folly::Function` is deliberately used instead of the proposed
`std::function`: normal FUSE submissions often capture move-only IO buffers,
and the same `Post` dispatch path must accept them without another heap
allocation or ownership indirection. A task submission keeps its own drain
admission through fiber completion, independently of the shorter-lived
EventBase notification admission. `Submit` first uses `Post` to admit the
dispatch; while the posted callback is still admitted, it transfers a new
pending-task reference to the business fiber. That transfer remains valid even
if shutdown has begun. A rejected `Post` rejects the request once, without
stranding an admitted task; a dispatch-allocation exception rolls back its
pending count before propagating to the caller.
EventBase callbacks must not throw: `Post` catches and logs exceptions from
the notification body, then releases the notification admission exactly once.
This isolates failed notifications without terminating the EventBase driver.
The `EventBase::runInEventBaseThread` enqueue API is itself `noexcept`; the
caller-side rollback guard protects only failures while preparing the
dispatch callback before entering that API.
Request cancellation is cooperative: each consumer decides its safe point.

## Scope and safety

This change provides request-token propagation, FUSE interrupt-to-token
mapping and runtime notification. It does not force-stop fibers, cancel Redis
transactions, undo committed writes or lock grants, or implement POSIX lock
waiter removal. #471/#472 consume this foundation when their blocking path
and reply/grant lifecycle are implemented. Interrupt callbacks cannot use a
`FiberRuntime*` after its lifetime; future consumers must own/guard the runtime
handle while their callbacks can remain registered.

Enabling FUSE interrupt reception does not automatically turn an interrupted
ordinary operation into `EINTR`: until a consumer implements a safe check,
the operation completes as before.

## Reference and trade-offs

JuiceFS carries an interrupt channel in its request context (`pkg/fuse/context.go`)
and checks it at selected VFS cancellation points. **Adopt:** request-local,
operation-agnostic cooperative cancellation. **Adapt:** use Folly's native
`CancellationSource/Token/Callback` rather than Go channels/context pooling;
dispatch wakeups through SwordFS's per-worker EventBase instead of blocking
its Redis worker. **Do not adopt:** a universal cancel-implies-failure rule,
because a request may have already committed a non-reversible operation.

## Verification

Unit contracts cover: default/copied tokens, previously arrived and concurrent
FUSE interrupts, no callback-side reply, hook unregistration before the FUSE
reply, overlapping cancellation callback/unregistration, failed consuming
replies, rejected request cleanup, `Post` execution domain, throwing
notification isolation, waiter wakeup, rejection and shutdown drain.
The GitHub development-only build verifies the test-first RED/GREEN behavior;
normal CI and per-file Codecov gates remain required before merge.
