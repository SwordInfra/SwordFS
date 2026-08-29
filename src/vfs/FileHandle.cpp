// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileHandle.hpp"

#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>

#include "utils/Logging.hpp"
#include "vfs/InodeHandle.hpp"

namespace swordfs::vfs {

// Concrete F14FastMap types — hidden from the header to avoid pulling
// the heavy Folly template into every includer.
struct FileMap : folly::F14FastMap<uint64_t, FileHandle> {};
struct DirMap : folly::F14FastMap<uint64_t, metadata::InodeID> {};

// ────────────────────────────────────────────────────────────────
// FileHandle
// ────────────────────────────────────────────────────────────────

FileHandle::FileHandle(std::shared_ptr<InodeHandle> handle)
    : fh_(FileHandleManager::Instance().AllocateFh()), handle_(std::move(handle)) {
}

utils::Status FileHandle::Open(metadata::InodeID ino, int flags, FileHandle *out) {
  auto inode_handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/true);
  if (!inode_handle) {
    return utils::Status::Internal("failed to get or create InodeHandle");
  }
  // Metadata fetch, permission check all live on the shared per-inode
  // InodeHandle.
  auto status = inode_handle->Open(flags);
  if (!status.ok()) {
    return status;
  }

  FileHandle handle{inode_handle};
  if (!FileHandleManager::Instance().Register(handle)) {
    // Roll back the +1 open-count from inode_handle->Open above.
    status = inode_handle->Close();
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileHandle::Open: rollback Close() failed: " << status.message();
    }
    return utils::Status::Internal("failed to register file handle");
  }
  *out = handle;
  return utils::Status::OK();
}

utils::Status FileHandle::Read(size_t size, off_t off, folly::IOBuf *out) {
  return handle_->Read(size, off, out);
}

utils::Status FileHandle::Write(const folly::IOBuf &buf, off_t off) {
  return handle_->Write(buf, off);
}

utils::Status FileHandle::Flush() {
  return handle_->Flush();
}

// ────────────────────────────────────────────────────────────────
// FileHandleManager
// ────────────────────────────────────────────────────────────────

FileHandleManager::FileHandleManager() : files_(std::make_unique<FileMap>()), dir_handles_(std::make_unique<DirMap>()) {
}

FileHandleManager &FileHandleManager::Instance() {
  static FileHandleManager instance;
  return instance;
}

uint64_t FileHandleManager::AllocateFh() {
  std::unique_lock lock(mutex_);
  return next_fh_++;
}

bool FileHandleManager::Register(FileHandle file_handle) {
  std::unique_lock lock(mutex_);
  auto [it, inserted] = files_->try_emplace(file_handle.fh(), std::move(file_handle));
  return inserted;
}

std::optional<FileHandle> FileHandleManager::Find(uint64_t fh) {
  std::shared_lock lock(mutex_);
  auto it = files_->find(fh);
  if (it != files_->end()) {
    return it->second;
  }
  return std::nullopt;
}

utils::Status FileHandleManager::Release(uint64_t fh) {
  std::shared_ptr<InodeHandle> inode_handle;
  {
    std::unique_lock lock(mutex_);
    auto it = files_->find(fh);
    if (it == files_->end()) {
      return utils::Status::OK();
    }
    inode_handle = it->second.handle();
    files_->erase(it);
  }
  return inode_handle->Close();
}

uint64_t FileHandleManager::OpenDir(metadata::InodeID ino) {
  uint64_t handle = AllocateFh();
  std::unique_lock lock(mutex_);
  (*dir_handles_)[handle] = ino;
  return handle;
}

void FileHandleManager::ReleaseDir(uint64_t fh) {
  std::unique_lock lock(mutex_);
  dir_handles_->erase(fh);
}

}  // namespace swordfs::vfs
