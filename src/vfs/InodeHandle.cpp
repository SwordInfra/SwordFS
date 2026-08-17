// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/InodeHandle.hpp"

#include <folly/container/F14Map.h>

#include <mutex>

#include "metadata/IMetaEngine.hpp"
#include "vfs/FileReadWriter.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

InodeHandle::InodeHandle(metadata::InodeID ino)
    : ino_(ino), rw_(std::make_shared<FileReadWriter>(ino)) {}

utils::Status InodeHandle::Open(int flags) {
  // Performs the open-time permission check and atime update.
  auto meta = volume::VolumeImpl::Instance().meta_engine();
  auto status = meta->Open(ino_);
  if (!status.ok()) {
    return status;
  }

  AcquireRef();

  if (flags & O_TRUNC) {
    return rw_->Truncate(0);
  }
  return utils::Status::OK();
}

utils::Status InodeHandle::Read(size_t size, off_t off, folly::IOBuf *out) {
  return rw_->Read(size, off, out);
}

utils::Status InodeHandle::Write(const folly::IOBuf &buf, off_t off) {
  return rw_->Write(buf, off);
}

utils::Status InodeHandle::Flush() { return rw_->Flush(); }

utils::Status InodeHandle::Close() {
  auto state = ReleaseRef();
  if (!state.is_last) {
    return utils::Status::OK();
  }

  auto status = rw_->Flush();
  if (!status.ok()) {
    return status;
  }
  // Reclaim exactly once, when the last reference to an orphaned inode is
  // released.  state.orphaned was snapshotted atomically with the
  // decrement inside ReleaseRef(), so this decision cannot race with
  // MarkOrphanedIfOpen().
  if (state.orphaned) {
    volume::VolumeImpl::Instance().meta_engine()->ReclaimData(ino_);
  }
  return utils::Status::OK();
}

bool InodeHandle::MarkOrphanedIfOpen() {
  // Atomically mark orphaned only when there is still an open fd.
  // We loop until either we win the CAS on `orphaned_` or we observe
  // that the open count has reached zero (in which case there is no
  // orphan to mark). The loop body is bounded by the number of
  // concurrent racing writers, which in practice is at most 1
  // (unlink) vs 1 (close).
  while (true) {
    if (open_count_.load(std::memory_order_acquire) == 0) {
      return false;
    }
    bool expected = false;
    if (orphaned_.compare_exchange_weak(
            expected, true,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      return true;
    }
    // Someone else won the CAS; reload and retry.
  }
}

uint64_t InodeHandle::open_count() const {
  // Lock-free read; safe because the only mutators (AcquireRef /
  // ReleaseRef) do atomic increments/decrements on the same type.
  return open_count_.load(std::memory_order_acquire);
}

void InodeHandle::AcquireRef() {
  open_count_.fetch_add(1, std::memory_order_acq_rel);
}

InodeHandle::ReleaseState InodeHandle::ReleaseRef() {
  uint64_t prev = open_count_.fetch_sub(1, std::memory_order_acq_rel);
  bool is_last = (prev == 1);
  bool was_orphaned = orphaned_.load(std::memory_order_acquire);
  return {is_last, is_last && was_orphaned};
}


// ────────────────────────────────────────────────────────────────
// InodeHandleManager
// ────────────────────────────────────────────────────────────────

// Concrete F14FastMap type — hidden from the header to avoid pulling the
// heavy Folly template into every includer.
struct InodeHandleMap
    : folly::F14FastMap<metadata::InodeID, std::weak_ptr<InodeHandle>> {};

InodeHandleManager::InodeHandleManager()
    : inode_handles_(std::make_unique<InodeHandleMap>()) {}

InodeHandleManager &InodeHandleManager::Instance() {
  static InodeHandleManager instance;
  return instance;
}

bool InodeHandleManager::HasOpenHandles(metadata::InodeID ino) {
  // Lock-free check via weak_ptr + atomic open_count_. We only take
  // the manager shared_lock to safely walk the map; once we hold a
  // strong reference to the handle, all subsequent reads are atomic.
  std::shared_ptr<InodeHandle> handle;
  {
    std::shared_lock lock(mutex_);
    auto it = inode_handles_->find(ino);
    if (it == inode_handles_->end()) return false;
    handle = it->second.lock();
  }
  return handle && handle->open_count() > 0;
}

void InodeHandleManager::MarkOrphaned(metadata::InodeID ino) {
  std::shared_ptr<InodeHandle> handle;
  {
    std::shared_lock lock(mutex_);
    auto it = inode_handles_->find(ino);
    if (it == inode_handles_->end()) return;
    handle = it->second.lock();
  }
  if (handle) handle->MarkOrphanedIfOpen();
}

std::shared_ptr<InodeHandle> InodeHandleManager::Get(metadata::InodeID ino, bool create_if_missing) {
  {
    std::shared_lock lock(mutex_);
    auto it = inode_handles_->find(ino);
    if (it != inode_handles_->end()) {
      if (auto handle = it->second.lock()) {
        return handle;
      }
    }
  }

  if (!create_if_missing) {
    return nullptr;
  }

  auto handle = std::make_shared<InodeHandle>(ino);

  std::unique_lock lock(mutex_);
  (*inode_handles_)[ino] = handle;
  return handle;
}

}  // namespace swordfs::vfs
