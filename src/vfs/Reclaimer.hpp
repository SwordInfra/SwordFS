// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Reclaimer — drives last-link data cleanup for the VFS layer.
//
// The metadata engine owns the durable state (orphan candidates published by
// unlink/rename-overwrite, plus frozen pending-reclaim records); this
// component owns the cross-engine sequence for one inode:
//
//   1. PrepareReclaim — the metadata point of no return: recheck nlink == 0,
//      freeze the authoritative object identities, drop the live inode;
//   2. delete every frozen object through the data engine, idempotently;
//   3. CompleteReclaim — drop the frozen record, only once every delete
//      succeeded.
//
// A failure between 1 and 3 therefore never loses data: the inode is already
// unreachable, and Reconcile() re-runs the remaining (idempotent) deletes at
// mount start, on foreground wakeups, and from periodic safety scans.

#pragma once

#include <folly/synchronization/LifoSem.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "metadata/Types.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::vfs {

class Reclaimer {
 public:
  static Reclaimer &Instance();

  Reclaimer(const Reclaimer &) = delete;
  Reclaimer &operator=(const Reclaimer &) = delete;

  // ────────────────────────────────────────────────────────────────
  // Reclaim (fiber domain)
  // ────────────────────────────────────────────────────────────────

  /// One reconciliation pass over crash-left state: promote orphan candidates
  /// whose nlink is still zero, and retry every frozen pending reclaim.
  /// Idempotent; production execution is serialized by the worker.
  utils::Status Reconcile();

  /// Start the background worker. Its first reconciliation pass runs
  /// immediately; subsequent passes run on Wake() or the periodic safety
  /// interval. Idempotent.
  void Start();

  /// Stop and join the worker. Idempotent and safe when never started.
  void Stop();

  /// Request an early reconciliation pass. Safe from both thread and fiber
  /// domains; duplicate wakeups are coalesced.
  void Wake();

 private:
  Reclaimer() = default;
  ~Reclaimer() = default;

  // Prepare an orphan after acquiring its local fence, then delete the frozen
  // work when preparation succeeds. A busy/open inode is left durable for a
  // later pass rather than treated as a failure.
  utils::Status PrepareOrphan(metadata::InodeID ino);

  // Delete every frozen object of |work| and complete the reclaim. Failures
  // are collected so every object is attempted; the record survives as long
  // as any delete failed.
  utils::Status DeleteFrozenObjects(const metadata::ReclaimWork &work);

  // Event/timeout loop of the POSIX worker, and one pass handed to the global
  // fiber runtime.
  void WorkerLoop();
  void RunWorkerPass();

 private:
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> wake_pending_{false};
  // Multi-post timed semaphore: foreground fibers only signal it, while the
  // POSIX worker consumes wakeups and also uses timeout as the safety scan.
  folly::LifoSem wake_sem_;
  // Thread-domain only: written by Start/Stop, which are called
  // from POSIX-thread lifecycle hooks (mount init/destroy and mount teardown).
  std::thread worker_thread_;
};

}  // namespace swordfs::vfs
