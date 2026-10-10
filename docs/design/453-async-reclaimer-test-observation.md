# Async Reclaimer test observation (#453)

## Problem and safety boundary

`WorkerSurvivesAFailedPassAndRecoversOnWake` drives a background
`ChunkGcWorker` thread plus fiber callbacks, while the assertion thread runs
its own test fiber. The pre-#453 `RecordingDataEngine` exposed mutable
`delete_calls` and `fail_keys` containers: its `Delete` callback read/wrote
those containers while the test polled/cleared them. The accesses had no
happens-before edge, so the test incurred C++ data races. Observing a pushed
Delete call alone also did not establish that a delete had *failed*.

## Test-only design

- The recording data engine owns its keys, objects, failure injection settings
  and call history in one `folly::Synchronized` state holder. Observations
  return snapshots, never references to mutable state. Fault changes share the
  same lock with Delete's lookup and bookkeeping.
- When Delete actually selects its injected failure, the engine posts an
  existing Folly fiber baton. The test waits on this semantic *failed Delete*
  event, rather than racing on an unprotected call-log count or adding a
  fixed sleep.
- The test also confirms the authoritative Redis pending-reclaim record
  remains present before recovery. It then clears the failure, explicitly
  wakes the **same still-running worker**, waits for durable reclaim removal,
  and stops/joins the worker before inspecting the final data-object state.
  A callback notification is not treated as proof that a whole worker pass
  has already finished; the asserted contract is the failed operation plus
  retained durable work, followed by recovery through the active worker.
- All other synchronous Reclaimer tests access the data engine through the
  same thread-safe test API, preventing future accidental unsynchronized
  observation during concurrent test evolution.

This introduces no production APIs, worker hooks, new retry policy, or
change to the filesystem's FileMetadata/ChunkMetadata authority boundary.
GitHub Debug/Release unit and service-backed CI remain the authoritative
verification for this test-infrastructure-only fix.
