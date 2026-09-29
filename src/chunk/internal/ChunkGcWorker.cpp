// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/internal/ChunkGcWorker.hpp"

#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include "chunk/cow/COWCleanup.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/FiberRuntime.hpp"
#include "utils/Logging.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::chunk::internal {
namespace {

constexpr auto kSafetyScanInterval = std::chrono::seconds(5);
constexpr size_t kPendingDeleteBatchSize = 128;

}  // namespace

utils::Status ChunkGcWorker::DeletePending(const metadata::PendingDelete &work) {
  if (chunk_type_ != metadata::ChunkType::kCow) {
    return utils::Status::NotSupported("chunk GC type is not implemented");
  }
  cow::COWRef ref;
  auto status = cow::DecodeCOWDelete(work, chunk_size_, &ref);
  if (!status.ok()) {
    return status;
  }

  metadata::SwordFsChunk current;
  status = meta_->FindChunk(ref.ino, ref.descriptor.index, &current);
  if (status.ok()) {
    if (current.revision == ref.descriptor.revision) {
      return utils::Status::OK();
    }
  } else if (!status.IsNotFound()) {
    return status;
  }

  status = data_->Delete(ref.key);
  if (!status.ok()) {
    return status;
  }
  return meta_->CompletePendingDelete(work.id);
}

utils::Status ChunkGcWorker::DeleteReclaim(const metadata::ReclaimWork &work) {
  if (chunk_type_ != metadata::ChunkType::kCow) {
    return utils::Status::NotSupported("chunk GC type is not implemented");
  }

  std::vector<cow::COWRef> refs;
  auto status = cow::DecodeCOWReclaim(work, chunk_size_, &refs);
  if (!status.ok()) {
    return status;
  }

  metadata::SwordFsInode inode;
  status = meta_->GetInode(work.ino, &inode);
  if (status.ok()) {
    // Redis EXEC may partially apply. Frozen work does not authorize a delete
    // while the live inode remains reachable.
    return utils::Status::Busy("reclaim inode is still live");
  }
  if (!status.IsNotFound()) {
    return status;
  }

  size_t failed = 0;
  for (const auto &ref : refs) {
    status = data_->Delete(ref.key);
    if (!status.ok()) {
      ++failed;
    }
  }
  if (failed != 0) {
    return utils::Status::IOError("COW reclaim left " + std::to_string(failed) + " object(s) undeleted");
  }
  return meta_->CompleteReclaim(work.ino);
}

utils::Status ChunkGcWorker::Reconcile() {
  utils::ExpectInFiberDomain();
  if (meta_ == nullptr || data_ == nullptr) {
    return utils::Status::OK();
  }

  size_t failures = 0;
  bool has_more_pending_deletes = false;
  auto status = meta_->VisitPendingDeletesBatch(
      kPendingDeleteBatchSize,
      [this, &failures](const metadata::PendingDelete &work) {
        auto status = DeletePending(work);
        if (!status.ok()) {
          ++failures;
          SWORDFS_LOG_WARN << "ChunkGcWorker: pending delete " << work.id << " failed: " << status.message();
        }
        return utils::Status::OK();
      },
      &has_more_pending_deletes);
  if (!status.ok()) {
    return status;
  }
  if (has_more_pending_deletes) {
    Wake();
  }

  status = meta_->VisitPendingReclaims([this, &failures](const metadata::ReclaimWork &work) {
    auto status = DeleteReclaim(work);
    if (!status.ok()) {
      ++failures;
      SWORDFS_LOG_WARN << "ChunkGcWorker: pending reclaim of ino " << work.ino << " failed: " << status.message();
    }
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }

  if (failures != 0) {
    return utils::Status::IOError("chunk GC left " + std::to_string(failures) + " cleanup item(s) pending");
  }
  return utils::Status::OK();
}

void ChunkGcWorker::Start() {
  utils::ExpectInThreadDomain();
  if (worker_thread_.joinable()) {
    return;
  }
  stop_requested_.store(false, std::memory_order_relaxed);
  wake_pending_.store(false, std::memory_order_relaxed);
  while (wake_sem_.try_wait()) {
  }
  worker_thread_ = std::thread([this] { WorkerLoop(); });
  SWORDFS_LOG_INFO << "chunk GC worker started";
}

void ChunkGcWorker::Stop() {
  utils::ExpectInThreadDomain();
  stop_requested_.store(true, std::memory_order_relaxed);
  if (!worker_thread_.joinable()) {
    return;
  }
  wake_sem_.post();
  worker_thread_.join();
  while (wake_sem_.try_wait()) {
  }
  wake_pending_.store(false, std::memory_order_relaxed);
  SWORDFS_LOG_INFO << "chunk GC worker stopped";
}

void ChunkGcWorker::Wake() {
  if (!wake_pending_.exchange(true, std::memory_order_acq_rel)) {
    wake_sem_.post();
  }
}

void ChunkGcWorker::WorkerLoop() {
  utils::ExpectInThreadDomain();
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    RunWorkerPass();
    if (stop_requested_.load(std::memory_order_relaxed)) {
      return;
    }
    if (wake_sem_.try_wait_for(kSafetyScanInterval)) {
      wake_pending_.store(false, std::memory_order_release);
    }
  }
}

void ChunkGcWorker::RunWorkerPass() {
  auto done = std::make_shared<utils::FiberBaton>();
  const bool submitted = utils::RunInFiber(
      [this, done] {
        auto post = folly::makeGuard([&] { done->post(); });
        try {
          auto status = Reconcile();
          if (!status.ok()) {
            SWORDFS_LOG_WARN << "chunk GC worker pass: " << status.message();
          }
        } catch (const std::exception &error) {
          SWORDFS_LOG_ERROR << "chunk GC worker pass threw: " << error.what();
        } catch (...) {
          SWORDFS_LOG_ERROR << "chunk GC worker pass threw an unknown exception";
        }
      },
      [done] { done->post(); });
  if (!submitted) {
    return;
  }
  done->wait();
}

}  // namespace swordfs::chunk::internal
