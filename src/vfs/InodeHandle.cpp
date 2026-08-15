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
  std::lock_guard<std::mutex> lock(state_mutex_);
  if (open_count_ == 0) {
    return false;
  }
  orphaned_ = true;
  return true;
}

uint64_t InodeHandle::open_count() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return open_count_;
}

void InodeHandle::AcquireRef() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  ++open_count_;
}

InodeHandle::ReleaseState InodeHandle::ReleaseRef() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  bool is_last = (--open_count_ == 0);
  return {is_last, is_last && orphaned_};
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
