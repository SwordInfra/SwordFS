// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/Reclaimer.hpp"

#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "chunk/IChunkOverwriteStrategy.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/FiberRuntime.hpp"
#include "utils/Logging.hpp"
#include "utils/Synchronization.hpp"
#include "vfs/InodeHandle.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

namespace {

// Safety-scan interval when no explicit wakeup arrives.
constexpr auto kSafetyScanInterval = std::chrono::seconds(5);
constexpr size_t kPendingDeleteBatchSize = 128;

}  // namespace

Reclaimer &Reclaimer::Instance() {
  static Reclaimer instance;
  return instance;
}

utils::Status Reclaimer::PrepareOrphan(metadata::InodeID ino) {
  utils::ExpectInFiberDomain();
  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  if (meta == nullptr) {
    return utils::Status::Internal("reclaim requires a metadata engine");
  }

  auto handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/true);
  if (!handle->TryStartReclaim()) {
    // A live/opening descriptor or another preparation attempt owns the local
    // inode. Durable orphan metadata remains authoritative for a later pass.
    return utils::Status::OK();
  }
  auto fence_guard = folly::makeGuard([&] { handle->FinishReclaim(); });

  std::optional<metadata::ReclaimWork> work;
  auto status = meta->PrepareReclaim(ino, &work);
  if (!status.ok()) {
    return status;
  }
  handle->FinishReclaim();
  fence_guard.dismiss();
  if (!work.has_value()) {
    return utils::Status::OK();
  }
  return DeleteFrozenObjects(*work);
}

utils::Status Reclaimer::DeleteFrozenObjects(const metadata::ReclaimWork &work) {
  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  auto *data = volume::VolumeImpl::Instance().data_engine();
  auto *strategy = volume::VolumeImpl::Instance().chunk_overwrite_strategy();
  auto status = strategy->DeleteFrozen(work, volume::VolumeImpl::Instance().chunk_size(), meta, data);
  if (!status.ok()) {
    return status;
  }
  return meta->CompleteReclaim(work.ino);
}

utils::Status Reclaimer::DeletePendingObject(const metadata::PendingDelete &work) {
  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  auto *data = volume::VolumeImpl::Instance().data_engine();
  auto *strategy = volume::VolumeImpl::Instance().chunk_overwrite_strategy();
  bool completed = false;
  auto status = strategy->DeletePending(work, volume::VolumeImpl::Instance().chunk_size(), meta, data, &completed);
  if (!status.ok()) {
    return status;
  }
  if (!completed) {
    return utils::Status::OK();
  }
  return meta->CompletePendingDelete(work.id);
}

utils::Status Reclaimer::Reconcile() {
  utils::ExpectInFiberDomain();

  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  auto *data = volume::VolumeImpl::Instance().data_engine();
  if (meta == nullptr || data == nullptr) {
    return utils::Status::OK();
  }

  size_t failures = 0;

  // The selected mechanism rechecks reachability before physical deletion.
  // Candidate age or queue position never grants delete authority.
  bool has_more_pending_deletes = false;
  auto status = meta->VisitPendingDeletesBatch(
      kPendingDeleteBatchSize,
      [this, &failures](const metadata::PendingDelete &work) {
        auto status = DeletePendingObject(work);
        if (!status.ok()) {
          ++failures;
          SWORDFS_LOG_WARN << "Reconcile: pending delete " << work.id << " failed: " << status.message();
        }
        return utils::Status::OK();
      },
      &has_more_pending_deletes);
  if (!status.ok()) {
    return status;
  }
  if (has_more_pending_deletes) {
    // Finish the rest of this pass first so pending inode reclaims and orphan
    // candidates are not starved, then let the worker consume this wakeup as
    // another bounded pass immediately afterwards.
    Wake();
  }

  // Prepared work already crossed the metadata point of no return. Recovery
  // can continue from the immutable record directly; DeleteFrozenObjects
  // independently fails closed if an inconsistent live inode still exists.
  status = meta->VisitPendingReclaims([this, &failures](const metadata::ReclaimWork &work) {
    auto status = DeleteFrozenObjects(work);
    if (!status.ok()) {
      ++failures;
      SWORDFS_LOG_WARN << "Reconcile: pending reclaim of ino " << work.ino << " failed: " << status.message();
    }
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }

  // Orphan candidates still have a live inode and may have local descriptors.
  // The InodeHandle fence is acquired only around preparation; physical
  // deletion uses the durable frozen record afterwards.
  status = meta->VisitOrphanCandidates([this, &failures](metadata::InodeID ino) {
    auto status = PrepareOrphan(ino);
    if (!status.ok()) {
      ++failures;
      SWORDFS_LOG_WARN << "Reconcile: orphan preparation of ino " << ino << " failed: " << status.message();
    }
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }

  if (failures != 0) {
    return utils::Status::IOError("reclaim reconciliation left " + std::to_string(failures) +
                                  " cleanup item(s) pending");
  }
  return utils::Status::OK();
}

void Reclaimer::Start() {
  utils::ExpectInThreadDomain();
  if (worker_thread_.joinable()) {
    return;
  }
  stop_requested_.store(false, std::memory_order_relaxed);
  wake_pending_.store(false, std::memory_order_relaxed);
  while (wake_sem_.try_wait()) {
  }
  worker_thread_ = std::thread([this] { WorkerLoop(); });
  SWORDFS_LOG_INFO << "reclaim worker started";
}

void Reclaimer::Stop() {
  utils::ExpectInThreadDomain();
  stop_requested_.store(true, std::memory_order_relaxed);
  if (worker_thread_.joinable()) {
    wake_sem_.post();
    worker_thread_.join();
    while (wake_sem_.try_wait()) {
    }
    wake_pending_.store(false, std::memory_order_relaxed);
    SWORDFS_LOG_INFO << "reclaim worker stopped";
  }
}

void Reclaimer::Wake() {
  if (!wake_pending_.exchange(true, std::memory_order_acq_rel)) {
    wake_sem_.post();
  }
}

void Reclaimer::WorkerLoop() {
  utils::ExpectInThreadDomain();
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    // Run once immediately at startup, then on an explicit Wake() or the
    // periodic safety interval. A wake that arrives while a pass is running is
    // retained by the semaphore for the next iteration.
    try {
      RunWorkerPass();
    } catch (const std::exception &error) {
      // Never let one pass kill the worker; the next pass retries.
      SWORDFS_LOG_ERROR << "reclaim worker pass threw: " << error.what();
    }
    if (stop_requested_.load(std::memory_order_relaxed)) {
      return;
    }
    if (wake_sem_.try_wait_for(kSafetyScanInterval)) {
      wake_pending_.store(false, std::memory_order_release);
    }
  }
}

void Reclaimer::RunWorkerPass() {
  // The deletes are data-engine calls and therefore fiber-domain work: hand
  // the pass to this thread's fiber runtime and wait for it to finish. The
  // rejection path posts the baton too, so this can never block forever.
  auto done = std::make_shared<utils::FiberBaton>();
  const bool submitted = utils::RunInFiber(
      [this, done] {
        auto post = folly::makeGuard([&] { done->post(); });
        // No exception may escape: it would unwind the fiber runtime's driver
        // loop, and the next pass would then enqueue a task nobody runs while
        // Stop() waits for this thread to join.
        try {
          auto status = Reconcile();
          if (!status.ok()) {
            SWORDFS_LOG_WARN << "reclaim worker pass: " << status.message();
          }
        } catch (const std::exception &error) {
          SWORDFS_LOG_ERROR << "reclaim worker pass threw: " << error.what();
        } catch (...) {
          SWORDFS_LOG_ERROR << "reclaim worker pass threw an unknown exception";
        }
      },
      [done] { done->post(); });
  if (!submitted) {
    return;
  }
  done->wait();
}

}  // namespace swordfs::vfs
