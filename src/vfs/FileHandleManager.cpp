// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileHandleManager.hpp"

#include <folly/container/F14Map.h>

#include "chunk/ChunkManager.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

// Concrete F14FastMap types — hidden from the header to avoid pulling
// the heavy Folly template into every includer.
struct FileMap : folly::F14FastMap<uint64_t, FileHandle> {};
struct DirMap : folly::F14FastMap<uint64_t, metadata::InodeID> {};

FileHandleManager::FileHandleManager()
    : files_(std::make_unique<FileMap>()),
      dir_handles_(std::make_unique<DirMap>()) {}

FileHandleManager &FileHandleManager::Instance() {
  static FileHandleManager instance;
  return instance;
}

uint64_t FileHandleManager::AllocFh() {
  std::unique_lock lock(mutex_);
  return next_fh_++;
}

utils::Status FileHandleManager::Open(metadata::InodeID ino,
                                      uint64_t *fh) {
  uint64_t handle = AllocFh();
  auto &vol = volume::VolumeImpl::Instance();
  auto rw = std::make_shared<FileReadWriter>(handle, ino, vol.chunk_size());
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
  {
    std::unique_lock lock(mutex_);
    auto it = files_->find(fh);
    if (it == files_->end()) return;
    handle = std::move(it->second);
    files_->erase(it);
  }
  // Flush outside the lock — I/O can take a long time.
  handle.file_readwriter->Flush();
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
