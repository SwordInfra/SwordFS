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

  size_t failed = 0;
  for (const auto &chunk : work.chunks) {
    // Only the frozen identity is used: never a key rebuilt from live state.
    auto status = data->Delete(chunk.key);
    if (!status.ok()) {
      ++failed;
      SWORDFS_LOG_ERROR << "Reclaim(" << work.ino << "): data->Delete(" << chunk.key
                        << ") failed: " << status.message();
    }
  }
  if (failed != 0) {
    // Keep the frozen record: the deletes are idempotent, so the next
    // reconciliation pass (or mount) finishes the job.
    return utils::Status::IOError("reclaim of inode " + std::to_string(work.ino) + " left " + std::to_string(failed) +
                                  " of " + std::to_string(work.chunks.size()) + " objects undeleted");
  }
  return meta->CompleteReclaim(work.ino);
}

utils::Status Reclaimer::Reconcile() {
  utils::ExpectInFiberDomain();

  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  auto *data = volume::VolumeImpl::Instance().data_engine();
  if (meta == nullptr || data == nullptr) {
    return utils::Status::OK();
  }

  size_t failures = 0;

  // Already-prepared work crossed the point of no return: the live inode no
  // longer exists, so no local open-fd fence is needed. Replay the frozen work
  // directly rather than going back through PrepareReclaim.
  auto status = meta->VisitPendingReclaims([this, &failures](const metadata::ReclaimWork &work) {
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
  // The InodeHandle fence is acquired only around preparation; object deletion
  // uses the durable frozen record afterwards.
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
    return utils::Status::IOError("reclaim reconciliation left " + std::to_string(failures) + " inode(s) unreclaimed");
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
