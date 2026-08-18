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
  // Reclaim exactly once, when the last reference to an orphaned inode
  // is released.  state.orphaned was snapshotted atomically with the
  // decrement inside ReleaseRef(), so this decision cannot race with
  // MarkOrphanedIfOpen(). ReclaimData removes both the chunk objects
  // (via the data engine) and the inode (via the metadata engine).
  if (state.orphaned) {
    InodeHandleManager::ReclaimData(ino_);
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

bool InodeHandleManager::HasOpenHandles(metadata::InodeID ino) {
  // Lock-free check via weak_ptr + atomic open_count_. We only take
  // the manager shared_lock to safely walk the map; once we hold a
  // strong reference to the handle, all subsequent reads are atomic.
  std::shared_ptr<InodeHandle> handle;
  {
    std::shared_lock lock(mutex_);
    auto it = inode_handles_->find(ino);
    if (it == inode_handles_->end()) {
      return false;
    }
    handle = it->second.lock();
  }
  return handle && handle->open_count() > 0;
}

void InodeHandleManager::MarkOrphaned(metadata::InodeID ino) {
  std::shared_ptr<InodeHandle> handle;
  {
    std::shared_lock lock(mutex_);
    auto it = inode_handles_->find(ino);
    if (it == inode_handles_->end()) {
      return;
    }
    handle = it->second.lock();
  }
  if (handle) {
    handle->MarkOrphanedIfOpen();
  }
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

utils::Status InodeHandleManager::ReclaimData(metadata::InodeID ino) {
  auto &volume = volume::VolumeImpl::Instance();
  auto *meta = volume.meta_engine();
  auto *data = volume.data_engine();

  // 1. Enumerate every chunk the metadata engine still tracks for this
  //    inode and issue a data-engine delete for each. Order matches
  //    insertion (chunk index ascending) so a log-based audit trail
  //    stays monotonic.
  std::vector<metadata::ChunkMeta> chunks;
  auto lc = meta->ListChunks(ino, &chunks);
  if (!lc.ok()) {
    SWORDFS_LOG_ERROR << "ReclaimData: ListChunks(" << ino
                      << ") failed: " << lc.message();
    return lc;
  }
  if (data) {
    for (const auto &cm : chunks) {
      // ChunkMeta::key is set by every writer (see Chunk::BuildMeta);
      // an empty key would indicate a back-end bug rather than normal
      // operation, so we trust it and surface it directly.
      auto st = data->Delete(cm.key);
      if (!st.ok()) {
        // Log but keep going — a background GC (TODO) is responsible
        // for stranded objects. The metadata view must converge
        // regardless.
        SWORDFS_LOG_ERROR << "ReclaimData: data->Delete(" << cm.key
                          << ") failed: " << st.message();
      }
    }
  } else if (!chunks.empty()) {
    SWORDFS_LOG_WARN << "ReclaimData: no data engine available; "
                     << chunks.size() << " chunk object(s) for ino "
                     << ino << " left for future GC";
  }

  // 2. Drop the inode from the metadata engine. This clears the chunk
  //    metadata map and the inode entry; once it returns, any
  //    subsequent Lookup returns ENOENT.
  return meta->ReclaimInode(ino);
}

}  // namespace swordfs::vfs
