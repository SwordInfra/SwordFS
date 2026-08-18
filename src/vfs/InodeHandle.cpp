// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/InodeHandle.hpp"

#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>

#include <mutex>

#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "vfs/FileReadWriter.hpp"
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

  // Reclaim exactly once, when the last reference to an orphaned inode
  // is released.  state.orphaned was snapshotted atomically with the
  // decrement inside ReleaseRef(), so this decision cannot race with
  // MarkOrphanedIfOpen(). ReclaimData removes both the chunk objects
  // (via the data engine) and the inode (via the metadata engine).
  if (state.orphaned) {
    auto status = ReclaimData();
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "InodeHandle::Close: ReclaimData(" << ino_
                        << ") failed: " << status.message();
    }
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

void InodeHandleManager::Initialize() {
  std::unique_lock lock(mutex_);
  inode_handles_->clear();
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

utils::Status InodeHandle::ReclaimData() {
  // Last-line-of-defence guards. Callers (VfsImpl::Unlink,
  // InodeHandle::Close) are supposed to verify these *before* they call
  // us, but they're racing with concurrent Link / Open on the same
  // inode and may have made a decision on stale state. ReclaimData is
  // the point of no return — once we Delete chunk objects from S3, no
  // other thread can recover them — so we re-check here.
  struct stat attr;
  Status status = meta_->GetAttr(ino_, &attr);
  if (status.IsNotFound()) {
    // Inode already gone — a concurrent reclaim won the race. The
    // chunk objects are presumably already deleted too. Idempotent
    // success.
    return utils::Status::OK();
  } else if (!status.ok()) {
    return status;
  }

  if (attr.st_nlink > 0) {
    // Another directory entry still references this inode (a
    // concurrent Link raced our Unlink). Refusing to reclaim keeps
    // the chunk objects alive for the surviving name.
    SWORDFS_LOG_WARN << "ReclaimData(" << ino_
                     << ") refused: nlink=" << attr.st_nlink
                     << " (>0). A concurrent Link won the race.";
    return utils::Status::OK();
  } else if (open_count_ > 0) {
    // An fd is still open on this inode. The caller should have
    // routed through MarkOrphaned instead — refuse to drop the
    // underlying chunks/inode out from under it.
    SWORDFS_LOG_WARN << "ReclaimData(" << ino_
                     << ") refused: open handle still references it.";
    return utils::Status::OK();
  }

  // 1. Enumerate every chunk the metadata engine still tracks for this
  //    inode and issue a data-engine delete for each.
  std::vector<metadata::ChunkMeta> chunks;
  status = meta_->ListChunks(ino_, &chunks);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "ReclaimData: ListChunks(" << ino_
                      << ") failed: " << status.message();
    return status;
  }
  for (const auto &cm : chunks) {
    status = data_->Delete(cm.key);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "ReclaimData: data->Delete(" << cm.key
                        << ") failed: " << status.message();
    }
  }

  // 2. Drop the inode from the metadata engine.
  return meta_->ReclaimInode(ino_);
}

}  // namespace swordfs::vfs
