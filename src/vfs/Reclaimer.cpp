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

// How often the periodic thread retries crash-left reclaim work.
constexpr auto kRetryInterval = std::chrono::seconds(5);

}  // namespace

Reclaimer &Reclaimer::Instance() {
  static Reclaimer instance;
  return instance;
}

utils::Status Reclaimer::Reclaim(metadata::InodeID ino, bool *prepared) {
  utils::ExpectInFiberDomain();
  if (prepared != nullptr) {
    *prepared = false;
  }

  auto *meta = volume::VolumeImpl::Instance().meta_engine();
  auto *data = volume::VolumeImpl::Instance().data_engine();
  if (meta == nullptr || data == nullptr) {
    // Without both planes nothing can be reclaimed; refusing keeps a frozen
    // record (if any) intact for a later mount instead of stranding objects.
    return utils::Status::Internal("reclaim requires a metadata and a data engine");
  }

  metadata::ReclaimWork work;
  auto status = meta->PrepareReclaim(ino, &work);
  if (status.IsNotFound()) {
    // The ordinary "nothing to reclaim" outcome: the inode was already
    // reclaimed, or a Link revived it. Nothing was changed, so it is not an
    // error — the caller only needs to know the point of no return was not
    // crossed.
    return utils::Status::OK();
  }
  if (!status.ok()) {
    return status;
  }
  if (prepared != nullptr) {
    *prepared = true;
  }
  return DeleteFrozenObjects(work);
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

  // Orphan candidates: an inode whose last name is gone but whose reclaim was
  // never prepared (crash, or a failed attempt). Claiming it through its
  // InodeHandle keeps a reopen of the unlinked inode working.
  std::vector<metadata::InodeID> orphans;
  auto status = meta->VisitOrphanCandidates([&orphans](metadata::InodeID ino) {
    orphans.push_back(ino);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }

  // Snapshot the pending queue as well before reclaiming anything: promoting
  // an orphan below can freeze it into a pending record, and processing that
  // record in this same pass would retry — and count — work this pass already
  // did.
  std::vector<metadata::InodeID> pending;
  status = meta->VisitPendingReclaims([&pending](metadata::InodeID ino) {
    pending.push_back(ino);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }

  for (metadata::InodeID ino : orphans) {
    if (!AttemptReclaim(ino).ok()) {
      ++failures;
    }
  }

  // Pending reclaims: the point of no return already passed, so only the
  // idempotent object deletes (and the completion) remain.
  for (metadata::InodeID ino : pending) {
    if (!AttemptReclaim(ino).ok()) {
      ++failures;
    }
  }

  if (failures != 0) {
    return utils::Status::IOError("reclaim reconciliation left " + std::to_string(failures) + " inode(s) unreclaimed");
  }
  return utils::Status::OK();
}

utils::Status Reclaimer::AttemptReclaim(metadata::InodeID ino) {
  auto handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/true);
  auto status = handle->ReclaimData();
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "Reconcile: reclaim of ino " << ino << " failed: " << status.message();
  }
  return status;
}

void Reclaimer::StartPeriodicRetry() {
  utils::ExpectInThreadDomain();
  if (retry_thread_.joinable()) {
    return;
  }
  stop_requested_.store(false, std::memory_order_relaxed);
  stop_waiter_.reset();
  retry_thread_ = std::thread([this] { RetryLoop(); });
  SWORDFS_LOG_INFO << "reclaim retry thread started";
}

void Reclaimer::StopPeriodicRetry() {
  utils::ExpectInThreadDomain();
  stop_requested_.store(true, std::memory_order_relaxed);
  if (retry_thread_.joinable()) {
    stop_waiter_.post();
    retry_thread_.join();
    SWORDFS_LOG_INFO << "reclaim retry thread stopped";
  }
}

void Reclaimer::RetryLoop() {
  utils::ExpectInThreadDomain();
  while (!stop_requested_.load(std::memory_order_relaxed)) {
    // On a POSIX thread FiberBaton uses its blocking-thread timed wait. Stop
    // posts the baton and wakes this immediately; timeout preserves the retry
    // cadence without adding shutdown latency.
    if (stop_waiter_.try_wait_for(kRetryInterval)) {
      return;
    }
    // A timed thread wait leaves the Baton in its consumed wait state. Reset
    // it before the next interval, then re-check the atomic stop flag so a
    // StopPeriodicRetry racing this timeout cannot be lost by the reset.
    stop_waiter_.reset();
    if (stop_requested_.load(std::memory_order_relaxed)) {
      return;
    }
    try {
      RunRetryPass();
    } catch (const std::exception &error) {
      // Never let one pass kill the retry thread; the next pass retries.
      SWORDFS_LOG_ERROR << "periodic reclaim retry pass threw: " << error.what();
    }
  }
}

void Reclaimer::RunRetryPass() {
  // The deletes are data-engine calls and therefore fiber-domain work: hand
  // the pass to this thread's fiber runtime and wait for it to finish. The
  // rejection path posts the baton too, so this can never block forever.
  auto done = std::make_shared<utils::FiberBaton>();
  const bool submitted = utils::RunInFiber(
      [this, done] {
        auto post = folly::makeGuard([&] { done->post(); });
        // No exception may escape: it would unwind the fiber runtime's driver
        // loop, and the next pass would then enqueue a task nobody runs while
        // StopPeriodicRetry waits for this thread to join.
        try {
          auto status = Reconcile();
          if (!status.ok()) {
            SWORDFS_LOG_WARN << "periodic reclaim retry: " << status.message();
          }
        } catch (const std::exception &error) {
          SWORDFS_LOG_ERROR << "periodic reclaim retry threw: " << error.what();
        } catch (...) {
          SWORDFS_LOG_ERROR << "periodic reclaim retry threw an unknown exception";
        }
      },
      [done] { done->post(); });
  if (!submitted) {
    return;
  }
  done->wait();
}

}  // namespace swordfs::vfs
