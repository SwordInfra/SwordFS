// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/synchronization/LifoSem.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <utility>

#include "utils/Status.hpp"

namespace swordfs::metadata {
class IMetaEngine;
struct PendingDelete;
struct ReclaimWork;
}  // namespace swordfs::metadata

namespace swordfs::chunk::internal {

class ChunkCleanupParticipant;

// Mount-private physical cleanup worker. Durable metadata queues are the
// restart work source; this class owns mechanism-neutral scheduling, retry,
// participant invocation and acknowledgement only.
class ChunkGcWorker {
 public:
  ChunkGcWorker(metadata::IMetaEngine *meta, std::unique_ptr<ChunkCleanupParticipant> cleanup)
      : meta_(meta), cleanup_(std::move(cleanup)) {
  }
  ~ChunkGcWorker();

  ChunkGcWorker(const ChunkGcWorker &) = delete;
  ChunkGcWorker &operator=(const ChunkGcWorker &) = delete;

  utils::Status Reconcile();
  void Start();
  void Stop();
  void Wake();

 private:
  utils::Status DeletePending(const metadata::PendingDelete &work);
  utils::Status DeleteReclaim(const metadata::ReclaimWork &work);
  void WorkerLoop();
  void RunWorkerPass();

 private:
  metadata::IMetaEngine *meta_;
  std::unique_ptr<ChunkCleanupParticipant> cleanup_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> wake_pending_{false};
  folly::LifoSem wake_sem_;
  std::thread worker_thread_;
};

}  // namespace swordfs::chunk::internal
