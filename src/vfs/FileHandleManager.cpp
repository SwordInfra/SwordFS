// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileHandleManager.hpp"

#include "chunk/ChunkManager.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

FileHandleManager &FileHandleManager::Instance() {
  static FileHandleManager instance;
  return instance;
}

utils::Status FileHandleManager::Open(uint64_t fh, volume::VolumeImpl *vol,
                                      metadata::InodeID ino) {
  chunk::ChunkManager::Instance().Init(vol->meta_engine(),
                                       vol->data_engine(),
                                       vol->chunk_size());
  auto rw = std::make_shared<FileReadWriter>(fh, ino, vol->chunk_size());
  std::unique_lock lock(mutex_);
  auto [it, inserted] = files_.try_emplace(fh, FileHandle{std::move(rw)});
  if (!inserted) {
    return utils::Status::AlreadyExists("fh " + std::to_string(fh) +
                                        " is already open");
  }
  return utils::Status::OK();
}

std::optional<FileHandle> FileHandleManager::Find(uint64_t fh) {
  std::shared_lock lock(mutex_);
  auto it = files_.find(fh);
  if (it != files_.end()) return it->second;
  return std::nullopt;
}

void FileHandleManager::Release(uint64_t fh) {
  FileHandle handle;
  {
    std::unique_lock lock(mutex_);
    auto it = files_.find(fh);
    if (it == files_.end()) return;
    handle = std::move(it->second);
    files_.erase(it);
  }
  // Flush outside the lock — I/O can take a long time.
  handle.file_readwriter->Flush();
}

}  // namespace swordfs::vfs
