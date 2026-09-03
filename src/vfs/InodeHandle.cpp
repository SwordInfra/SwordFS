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
#include "vfs/GarbageCollector.hpp"
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
  // Reserve the local reference first so same-mount unlink cannot reclaim
  // while the persistent open reference is being published. AcquireOpen is
  // the cross-mount linearization point: it races atomically with reclaim on
  // the inode/open-count keys.
  AcquireRef();

  auto status = meta_->AcquireOpen(ino_);
  if (!status.ok()) {
    ReleaseFailedOpen(/*persistent_acquired=*/false);
    return status;
  }

  // Permission/type checks and the atime update happen after the durable open
  // reference exists. If they fail, rollback both reference layers.
  status = meta_->Open(ino_);
  if (!status.ok()) {
    ReleaseFailedOpen(/*persistent_acquired=*/true);
    return status;
  }

  if (flags & O_TRUNC) {
    status = rw_->Truncate(0);
    if (!status.ok()) {
      ReleaseFailedOpen(/*persistent_acquired=*/true);
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

utils::Status InodeHandle::Flush() {
  return rw_->Flush();
}

utils::Status InodeHandle::Close() {
  ReleaseState state{};
  bool reclaimable = false;
  utils::Status flush_status = utils::Status::OK();
  utils::Status release_status = utils::Status::OK();
  {
    // Keep both the local and persistent open references alive while the final
    // close flushes dirty data. Holding state_mutex_ through ReleaseOpen also
    // prevents a same-mount Open from appearing between the persistent release
    // and the local count transition.
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (open_count_ == 0) {
      return utils::Status::Internal("closing inode with no open references");
    }
    if (open_count_ == 1) {
      flush_status = rw_->Flush();
    }

    release_status = meta_->ReleaseOpen(ino_, &reclaimable);
    if (!release_status.ok()) {
      SWORDFS_LOG_ERROR << "InodeHandle::Close: ReleaseOpen(" << ino_ << ") failed: " << release_status.message();
    }

    --open_count_;
    const bool is_last = open_count_ == 0;
    state = {is_last, is_last && orphaned_};
  }

  // A local unlink marks orphaned_, while a remote unlink is detected by the
  // filesystem-wide open-count transition returned from ReleaseOpen(). If the
  // persistent release failed, ReclaimInode's global-open guard will keep the
  // inode alive; the session cleanup path can conservatively release it later.
  if ((state.is_last && state.orphaned) || reclaimable) {
    auto reclaim_status = ReclaimData();
    if (!reclaim_status.ok()) {
      SWORDFS_LOG_ERROR << "InodeHandle::Close: ReclaimData(" << ino_ << ") failed: " << reclaim_status.message();
    }
  }

  if (!flush_status.ok()) {
    return flush_status;
  }
  return release_status;
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
  if (open_count_ == 0) {
    return {false, false};
  }
  bool is_last = (--open_count_ == 0);
  return {is_last, is_last && orphaned_};
}

void InodeHandle::ReleaseFailedOpen(bool persistent_acquired) {
  bool reclaimable = false;
  if (persistent_acquired) {
    auto status = meta_->ReleaseOpen(ino_, &reclaimable);
    if (!status.ok()) {
      SWORDFS_LOG_WARN << "InodeHandle::Open rollback: ReleaseOpen(" << ino_ << ") failed: " << status.message();
    }
  }

  auto state = ReleaseRef();
  if (!state.orphaned && !reclaimable) {
    return;
  }
  auto status = ReclaimData();
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "InodeHandle::Open rollback: ReclaimData(" << ino_ << ") failed: " << status.message();
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
  std::unique_lock lock(mutex_);
  inode_handles_->clear();
}

std::shared_ptr<InodeHandle> InodeHandleManager::Get(metadata::InodeID ino, bool create_if_missing) {
  std::unique_lock lock(mutex_);
  auto it = inode_handles_->find(ino);
  if (it != inode_handles_->end()) {
    if (auto handle = it->second.lock()) {
      return handle;
    }
  }

  if (!create_if_missing) {
    return nullptr;
  }

  auto handle = std::make_shared<InodeHandle>(ino);
  (*inode_handles_)[ino] = handle;
  return handle;
}

utils::Status InodeHandle::ReclaimData() {
  // Serialize the no-open check with AcquireRef(). Holding this lock through
  // preparation and object deletion is intentional: once we decide an inode
  // is reclaimable, no local Open may slip in before ReclaimInode publishes
  // the durable deletion job.
  std::unique_lock<std::mutex> lock(state_mutex_);
  if (open_count_ > 0) {
    SWORDFS_LOG_WARN << "ReclaimData(" << ino_ << ") refused: open handle still references it.";
    return utils::Status::OK();
  }

  metadata::SwordFsInode inode;
  Status status = meta_->GetInode(ino_, &inode);
  if (status.IsNotFound()) {
    // A previous attempt may already have removed the live inode while its
    // durable deletion job is still pending. Drive that job again instead of
    // assuming the data objects disappeared with the inode metadata.
    GarbageCollector collector(meta_, data_);
    return collector.Reclaim(ino_);
  } else if (!status.ok()) {
    return status;
  }

  if (inode.attr.nlink > 0) {
    // Another directory entry still references this inode (a concurrent Link
    // raced our Unlink). Refusing to reclaim keeps the chunk objects alive for
    // the surviving name.
    SWORDFS_LOG_WARN << "ReclaimData(" << ino_ << ") refused: nlink=" << inode.attr.nlink
                     << " (>0). A concurrent Link won the race.";
    return utils::Status::OK();
  }

  // Persist the deletion job and freeze its chunk list before deleting any
  // data object. A failed or interrupted delete leaves the job discoverable
  // for the mount-time and periodic reconciler.
  GarbageCollector collector(meta_, data_);
  return collector.Reclaim(ino_);
}

}  // namespace swordfs::vfs
