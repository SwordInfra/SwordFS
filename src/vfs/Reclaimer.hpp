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
// mount time and from a periodic retry thread.

#pragma once

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

  /// Prepare, delete and complete the reclaim of |ino|.
  ///
  /// |*prepared| reports whether the metadata engine crossed the point of no
  /// return. When it is false nothing was changed — the inode was already
  /// reclaimed, or a concurrent Link revived it. When it is true the live
  /// inode is already gone, so the caller may release any local fencing on
  /// return; a failed delete leaves a frozen pending record that
  /// reconciliation retries, and an open on the (now unlinked) inode fails at
  /// the metadata engine rather than reading deleted objects.
  utils::Status Reclaim(metadata::InodeID ino, bool *prepared);

  /// One reconciliation pass over crash-left state: promote orphan candidates
  /// whose nlink is still zero, and retry every frozen pending reclaim.
  /// Idempotent and safe to run concurrently with runtime reclaims.
  utils::Status Reconcile();

  // ────────────────────────────────────────────────────────────────
  // Periodic retry (thread domain, production mount only)
  // ────────────────────────────────────────────────────────────────

  /// Start the periodic retry thread. Idempotent.
  void StartPeriodicRetry();

  /// Stop and join the periodic retry thread. Idempotent, safe when no thread
  /// was ever started, and must run before the engines are torn down.
  void StopPeriodicRetry();

 private:
  Reclaimer() = default;
  ~Reclaimer() = default;

  // Delete every frozen object of |work| and complete the reclaim. Failures
  // are collected so every object is attempted; the record survives as long
  // as any delete failed.
  utils::Status DeleteFrozenObjects(const metadata::ReclaimWork &work);

  // Reclaim |ino| through its InodeHandle, so an open descriptor still defers
  // the cleanup to the last Close instead of losing the data under it.
  utils::Status AttemptReclaim(metadata::InodeID ino);

  // Sleep/retry loop of the periodic thread, and one pass handed to the fiber
  // runtime of that thread.
  void RetryLoop();
  void RunRetryPass();

 private:
  std::atomic<bool> stop_requested_{false};
  // Cross-domain wake-up for the POSIX retry thread. The timed wait provides
  // the retry interval; StopPeriodicRetry posts it so shutdown never waits
  // for a polling sleep to expire.
  utils::FiberBaton stop_waiter_;
  // Thread-domain only: written by Start/StopPeriodicRetry, which are called
  // from POSIX-thread lifecycle hooks (mount init/destroy and mount teardown).
  std::thread retry_thread_;
};

}  // namespace swordfs::vfs
