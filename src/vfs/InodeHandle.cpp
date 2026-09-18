// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/InodeHandle.hpp"

#include <folly/ScopeGuard.h>
#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>

#include <mutex>

#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "vfs/FileReadWriter.hpp"
#include "vfs/Reclaimer.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

InodeHandle::InodeHandle(metadata::InodeID ino)
    : ino_(ino),
      meta_(volume::VolumeImpl::Instance().meta_engine()),
      data_(volume::VolumeImpl::Instance().data_engine()),
      rw_(std::make_shared<FileReadWriter>(ino)) {
  CHECK(data_ != nullptr);
}

utils::Status InodeHandle::Open(int flags) {
  // Take the descriptor reference before the metadata check: the reclaim
  // fence is claimed under the same lock, so a concurrent reclaim either
  // sees this reference (and refuses) or this caller sees the fence (and
  // fails) — an inode can never be frozen while a descriptor is being opened
  // on it.
  if (!AcquireRefUnlessReclaiming()) {
    return utils::Status::NotFound("inode is being reclaimed");
  }

  // Performs the open-time permission check and atime update.
  auto meta = volume::VolumeImpl::Instance().meta_engine();
  auto status = meta->Open(ino_);
  if (!status.ok()) {
    ReleaseRefAfterFailedOpen();
    return status;
  }

  if (flags & O_TRUNC) {
    status = rw_->Truncate(0);
    if (!status.ok()) {
      ReleaseRefAfterFailedOpen();
      return status;
    }
  }
  return utils::Status::OK();
}

utils::Status InodeHandle::Read(size_t size, off_t off, folly::IOBuf *out) {
  return rw_->Read(size, off, out);
}

utils::Status InodeHandle::Write(const folly::IOBuf &buf, off_t off) {
  return rw_->Write(buf, off);
}

utils::Status InodeHandle::SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields,
                                   metadata::SwordFsInode *out) {
  return rw_->SetAttr(attr, fields, out);
}

utils::Status InodeHandle::Flush() {
  return rw_->Flush();
}

utils::Status InodeHandle::Close() {
  if (!PrepareClose()) {
    return utils::Status::OK();
  }

  auto status = rw_->Flush();
  // Keep the closing descriptor counted until the flush above completes. A
  // concurrent Open can therefore acquire a normal reference while a
  // concurrent unlink/reconcile sees this descriptor and defers reclaim.
  // Only after the flush do we drop our reference and decide whether this is
  // still the last descriptor.
  auto state = ReleaseRef(/*allow_reclaim=*/status.ok());
  if (!status.ok()) {
    // The descriptor is closed even when flushing failed, but this close path
    // must not reclaim data that failed to publish. A durable orphan candidate
    // (if any) remains available for later reconciliation.
    return status;
  }

  // Reclaim exactly once when this flush was still the last live descriptor
  // and an unlink/reconciliation marked the inode orphaned while it was open.
  // ReleaseRef snapshots that flag and claims the fence together with the
  // final decrement, so no competing reclaim can slip between the decision
  // and the metadata point of no return.
  // fence_claimed implies orphaned: ReleaseRef only claims the reclaim fence
  // when the final reference observed local orphan deferral. Do not repeat
  // that invariant as a second condition here; it only creates an impossible
  // state combination for readers and coverage alike.
  if (state.fence_claimed) {
    auto reclaim_status = ReclaimWithFence();
    if (!reclaim_status.ok()) {
      SWORDFS_LOG_ERROR << "InodeHandle::Close: reclaim of " << ino_ << " failed: " << reclaim_status.message();
    }
    return utils::Status::OK();
  }
  return utils::Status::OK();
}

uint64_t InodeHandle::open_count() const {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  return open_count_;
}

bool InodeHandle::AcquireRefUnlessReclaiming() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  if (reclaim_started_) {
    return false;
  }
  ++open_count_;
  return true;
}

bool InodeHandle::PrepareClose() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  CHECK_GT(open_count_, 0) << "InodeHandle::PrepareClose without a matching Open on inode " << ino_;
  if (open_count_ == 1) {
    // Keep the last descriptor counted until its Flush completes. This is the
    // key lifetime invariant: close-in-progress is still a live reference.
    return true;
  }
  --open_count_;
  return false;
}

InodeHandle::ReleaseState InodeHandle::ReleaseRef(bool allow_reclaim) {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  // Every release is paired with a successful AcquireRefUnlessReclaiming(). An
  // underflow would wrap the count to a huge value and silently defer every
  // later reclaim of this inode forever, so catch the imbalance where it is.
  CHECK_GT(open_count_, 0) << "InodeHandle::ReleaseRef without a matching Open on inode " << ino_;
  const bool is_last = (--open_count_ == 0);
  const bool was_orphaned = is_last && orphaned_;
  if (is_last) {
    // orphaned_ is only local open-fd deferral state. Once no descriptor is
    // left, the durable metadata orphan/pending record is the recovery
    // authority, so do not let a stale local flag survive a Link revival or a
    // failed/declined reclaim and affect later opens/closes of this inode.
    orphaned_ = false;
  }
  ReleaseState state{is_last, was_orphaned, false};
  if (is_last && allow_reclaim && was_orphaned) {
    // Claim the fence together with the final decrement: from this point a
    // concurrent Open fails until the metadata point of no return resolves.
    // reclaim_started_ cannot already be true here: a reclaim only claims the
    // fence while open_count_ is zero, whereas this path just released a live
    // descriptor reference.
    reclaim_started_ = true;
    state.fence_claimed = true;
  }
  return state;
}

void InodeHandle::ReleaseReclaim() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  reclaim_started_ = false;
}

utils::Status InodeHandle::ReclaimWithFence() {
  // The fence must be released on every exit path — including an exception
  // from a backend that throws instead of returning a Status (an S3 delete,
  // an allocation failure). A leaked fence would refuse every later reclaim
  // of this inode, so its objects could never be deleted.
  //
  // Releasing it is safe: it has done its job by closing the window between
  // the caller's decision and the metadata point of no return, and afterwards
  // a revived inode must stay openable while a reclaimed inode no longer
  // exists (an open on it fails at the metadata engine). Releasing also lets
  // a retry (periodic reconciliation, or a later call) run through this
  // handle instead of being refused forever.
  auto fence_guard = folly::makeGuard([this] { ReleaseReclaim(); });

  // The metadata engine rechecks nlink == 0 in the same mutation that freezes
  // the object identities and drops the inode, so a Link that raced us either
  // lands before that mutation (and cancels the reclaim) or fails NotFound.
  bool prepared = false;
  auto status = Reclaimer::Instance().Reclaim(ino_, &prepared);
  (void)prepared;
  return status;
}

void InodeHandle::ReleaseRefAfterFailedOpen() {
  auto state = ReleaseRef(/*allow_reclaim=*/true);
  if (!state.is_last) {
    return;
  }
  // As in Close(), a claimed fence already proves this was the final orphaned
  // reference. Keep the state transition single-sourced in ReleaseRef().
  if (state.fence_claimed) {
    // The inode was unlinked while this open was in flight and no descriptor
    // is left: finish the cleanup now instead of leaving it to reconciliation.
    auto status = ReclaimWithFence();
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "InodeHandle::Open: reclaim of " << ino_
                        << " after a failed open failed: " << status.message();
    }
    return;
  }
}

// ────────────────────────────────────────────────────────────────
// InodeHandleManager
// ────────────────────────────────────────────────────────────────

// Concrete F14FastMap type — hidden from the header to avoid pulling the
// heavy Folly template into every includer.
struct InodeHandleMap : folly::F14FastMap<metadata::InodeID, std::weak_ptr<InodeHandle>> {};

InodeHandleManager::InodeHandleManager() : inode_handles_(std::make_unique<InodeHandleMap>()) {
}

InodeHandleManager &InodeHandleManager::Instance() {
  static InodeHandleManager instance;
  return instance;
}

void InodeHandleManager::Initialize() {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  inode_handles_->clear();
}

std::shared_ptr<InodeHandle> InodeHandleManager::Get(metadata::InodeID ino, bool create_if_missing) {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = inode_handles_->find(ino);
  if (it != inode_handles_->end()) {
    if (auto handle = it->second.lock()) {
      return handle;
    }
    // The entry expired (all references dropped): drop it so the registry
    // does not grow without bound across a mount's lifetime. Reclaim
    // reconciliation visits one inode at a time, so this matters there.
    inode_handles_->erase(it);
  }

  if (!create_if_missing) {
    return nullptr;
  }

  auto handle = std::make_shared<InodeHandle>(ino);
  (*inode_handles_)[ino] = handle;
  return handle;
}

utils::Status InodeHandle::ReclaimData() {
  {
    // Defer or reclaim — one critical section, so the decision can never be
    // taken on a stale open count. A concurrent Open either already holds a
    // descriptor reference (and is visible here) or arrives after
    // reclaim_started_ is set (and Open then fails NotFound), so no descriptor
    // can slip onto an inode whose objects are about to be frozen and deleted.
    std::lock_guard<utils::FiberMutex> lock(state_mutex_);
    if (open_count_ > 0) {
      // A descriptor still references the inode. Mark it orphaned on this
      // handle so the last Close() reclaims it; the reader keeps working until
      // then.
      orphaned_ = true;
      SWORDFS_LOG_DEBUG << "ReclaimData(" << ino_ << ") deferred to the last descriptor.";
      return utils::Status::OK();
    }
    if (reclaim_started_) {
      // Another reclaim — or the final Close of an orphaned inode — already
      // owns the fence and is responsible for the cleanup. With no live
      // descriptor there is nothing to defer locally; the durable metadata
      // orphan/pending record remains the retry authority if that owner does
      // not finish the cleanup.
      SWORDFS_LOG_DEBUG << "ReclaimData(" << ino_ << ") deferred: another reclaim owns the fence.";
      return utils::Status::OK();
    }
    // Claim the local point of no return. The fence is released by
    // ReclaimWithFence() on every exit path.
    reclaim_started_ = true;
  }
  return ReclaimWithFence();
}

}  // namespace swordfs::vfs
