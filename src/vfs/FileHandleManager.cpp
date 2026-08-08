// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileHandleManager.hpp"

#include <folly/container/F14Map.h>

namespace swordfs::vfs {

// Concrete F14FastMap types — hidden from the header to avoid pulling
// the heavy Folly template into every includer.
struct FileMap : folly::F14FastMap<uint64_t, FileHandle> {};
struct DirMap : folly::F14FastMap<uint64_t, metadata::InodeID> {};
struct InodeWriterMap : folly::F14FastMap<metadata::InodeID, std::weak_ptr<FileReadWriter>> {};

FileHandleManager::FileHandleManager()
    : files_(std::make_unique<FileMap>()),
      dir_handles_(std::make_unique<DirMap>()),
      inode_writers_(std::make_unique<InodeWriterMap>()) {}

FileHandleManager &FileHandleManager::Instance() {
  static FileHandleManager instance;
  return instance;
}

uint64_t FileHandleManager::AllocFh() {
  std::unique_lock lock(mutex_);
  return next_fh_++;
}

std::shared_ptr<FileReadWriter> FileHandleManager::GetFileReadWriter(
    metadata::InodeID ino, bool create_if_missing) {
  {
    std::shared_lock lock(mutex_);
    auto it = inode_writers_->find(ino);
    if (it != inode_writers_->end()) {
      if (auto rw = it->second.lock()) return rw;
    }
  }
  if (!create_if_missing) return nullptr;

  auto rw = std::make_shared<FileReadWriter>(ino);
  std::unique_lock lock(mutex_);
  (*inode_writers_)[ino] = rw;
  return rw;
}

utils::Status FileHandleManager::Open(metadata::InodeID ino,
                                      uint64_t *fh) {
  uint64_t handle = AllocFh();
  auto rw = GetFileReadWriter(ino, /*create_if_missing=*/true);

  std::unique_lock lock(mutex_);
  auto [it, inserted] = files_->try_emplace(handle, FileHandle{std::move(rw)});
  if (!inserted) {
    return utils::Status::AlreadyExists("fh " + std::to_string(handle) +
                                        " is already open");
  }
  *fh = handle;
  return utils::Status::OK();
}

std::optional<FileHandle> FileHandleManager::Find(uint64_t fh) {
  std::shared_lock lock(mutex_);
  auto it = files_->find(fh);
  if (it != files_->end()) return it->second;
  return std::nullopt;
}

void FileHandleManager::Release(uint64_t fh) {
  FileHandle handle;
  bool last_ref = false;
  {
    std::unique_lock lock(mutex_);
    auto it = files_->find(fh);
    if (it == files_->end()) return;
    // Check if this is the last fh referencing this FileReadWriter.
    if (it->second.file_readwriter.use_count() == 1) {
      last_ref = true;
    }
    handle = std::move(it->second);
    files_->erase(it);
  }
  // Only flush when the last file handle for this inode is released.
  if (last_ref) {
    handle.file_readwriter->Flush();
  }
}

uint64_t FileHandleManager::OpenDir(metadata::InodeID ino) {
  uint64_t handle = AllocFh();
  std::unique_lock lock(mutex_);
  (*dir_handles_)[handle] = ino;
  return handle;
}

void FileHandleManager::ReleaseDir(uint64_t fh) {
  std::unique_lock lock(mutex_);
  dir_handles_->erase(fh);
}

}  // namespace swordfs::vfs
