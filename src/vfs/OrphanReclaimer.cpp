// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/OrphanReclaimer.hpp"

#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string>

#include "metadata/IMetaEngine.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/FiberRuntime.hpp"
#include "utils/Logging.hpp"
#include "utils/Synchronization.hpp"
#include "vfs/InodeHandle.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

constexpr auto kSafetyScanInterval = std::chrono::seconds(5);
constexpr size_t kOrphanBatchSize = 128;

}  // namespace

OrphanReclaimer &OrphanReclaimer::Instance() {
  static OrphanReclaimer instance;
  return instance;
}

utils::Status OrphanReclaimer::PrepareOrphan(metadata::InodeID ino) {
  utils::ExpectInFiberDomain();
  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  // Reconcile() resolves the mount metadata engine before visiting candidates;
  // PrepareOrphan is private so it cannot be called outside that pass.
  CHECK(meta != nullptr);

  auto handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/true);
  if (!handle->TryStartReclaim()) {
    return utils::Status::OK();
  }
  auto fence_guard = folly::makeGuard([&] { handle->FinishReclaim(); });
  auto status = meta->PrepareReclaim(ino);
  if (!status.ok()) {
    return status;
  }
  handle->FinishReclaim();
  fence_guard.dismiss();
  return utils::Status::OK();
}

utils::Status OrphanReclaimer::Reconcile() {
  utils::ExpectInFiberDomain();
  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  if (meta == nullptr) {
    return utils::Status::OK();
  }

  size_t failures = 0;
  size_t visited = 0;
  bool batch_limited = false;
  bool stopping = false;
  auto status =
      meta->VisitOrphanCandidates([this, &failures, &visited, &batch_limited, &stopping](metadata::InodeID ino) {
        if (stop_requested_.load(std::memory_order_relaxed)) {
          stopping = true;
          return utils::Status::Busy("orphan reclaim stopping");
        }
        if (visited >= kOrphanBatchSize) {
          batch_limited = true;
          return utils::Status::Busy("orphan reclaim batch complete");
        }
        ++visited;
        auto status = PrepareOrphan(ino);
        if (!status.ok()) {
          ++failures;
          SWORDFS_LOG_WARN << "OrphanReclaimer: preparation of ino " << ino << " failed: " << status.message();
        }
        return utils::Status::OK();
      });
  if (stopping) {
    return utils::Status::OK();
  }
  if (!status.ok() && !batch_limited) {
    return status;
  }
  if (batch_limited) {
    // The metadata visitor preserves its continuation point when our callback
    // stops a pass. Schedule the next bounded slice immediately instead of
    // waiting for the periodic safety scan.
    Wake();
  }
  if (failures != 0) {
    return utils::Status::IOError("orphan reclaim left " + std::to_string(failures) + " inode(s) pending");
  }
  return utils::Status::OK();
}

void OrphanReclaimer::Start() {
  utils::ExpectInThreadDomain();
  if (worker_thread_.joinable()) {
    return;
  }
  stop_requested_.store(false, std::memory_order_relaxed);
  wake_pending_.store(false, std::memory_order_relaxed);
  while (wake_sem_.try_wait()) {
  }
  worker_thread_ = std::thread([this] { WorkerLoop(); });
  SWORDFS_LOG_INFO << "orphan reclaimer started";
}

void OrphanReclaimer::Stop() {
  utils::ExpectInThreadDomain();
  if (!worker_thread_.joinable()) {
    stop_requested_.store(false, std::memory_order_relaxed);
    return;
  }
  stop_requested_.store(true, std::memory_order_relaxed);
  wake_sem_.post();
  worker_thread_.join();
  while (wake_sem_.try_wait()) {
  }
  wake_pending_.store(false, std::memory_order_relaxed);
  stop_requested_.store(false, std::memory_order_relaxed);
  SWORDFS_LOG_INFO << "orphan reclaimer stopped";
}

void OrphanReclaimer::Wake() {
  if (!wake_pending_.exchange(true, std::memory_order_acq_rel)) {
    wake_sem_.post();
  }
}

void OrphanReclaimer::WorkerLoop() {
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

void OrphanReclaimer::RunWorkerPass() {
  auto done = std::make_shared<utils::FiberBaton>();
  const bool submitted = utils::RunInFiber(
      [this, done] {
        auto post = folly::makeGuard([&] { done->post(); });
        try {
          auto status = Reconcile();
          if (!status.ok()) {
            SWORDFS_LOG_WARN << "orphan reclaimer pass: " << status.message();
          }
        } catch (const std::exception &error) {
          SWORDFS_LOG_ERROR << "orphan reclaimer pass threw: " << error.what();
        } catch (...) {
          SWORDFS_LOG_ERROR << "orphan reclaimer pass threw an unknown exception";
        }
      },
      [done] { done->post(); });
  if (!submitted) {
    return;
  }
  done->wait();
}

}  // namespace swordfs::vfs
