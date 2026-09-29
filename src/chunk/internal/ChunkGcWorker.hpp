// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/synchronization/LifoSem.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {
class IMetaEngine;
struct PendingDelete;
struct ReclaimWork;
}  // namespace swordfs::metadata

namespace swordfs::storage {
class IDataEngine;
}

namespace swordfs::chunk::internal {

// Mount-private physical cleanup worker. Durable metadata queues are the
// restart authority; this class owns mechanism-specific decode, reachability
// validation, data deletion and acknowledgement.
class ChunkGcWorker {
 public:
  ChunkGcWorker(metadata::ChunkOverwriteMechanism mechanism, uint64_t chunk_size, metadata::IMetaEngine *meta,
                storage::IDataEngine *data)
      : mechanism_(mechanism), chunk_size_(chunk_size), meta_(meta), data_(data) {
  }

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
  metadata::ChunkOverwriteMechanism mechanism_;
  uint64_t chunk_size_;
  metadata::IMetaEngine *meta_;
  storage::IDataEngine *data_;
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> wake_pending_{false};
  folly::LifoSem wake_sem_;
  std::thread worker_thread_;
};

}  // namespace swordfs::chunk::internal
