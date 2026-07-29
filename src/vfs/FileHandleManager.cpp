// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileHandleManager.hpp"

#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

FileHandleManager &FileHandleManager::Instance() {
  static FileHandleManager instance;
  return instance;
}

utils::Status FileHandleManager::Open(uint64_t fh, volume::VolumeImpl *vol,
                             metadata::InodeID ino) {
  auto rw = std::make_unique<FileReadWriter>(
      vol->meta_engine(), vol->data_engine(), ino);
  std::unique_lock lock(mutex_);
  auto [it, inserted] = files_.try_emplace(fh, std::move(rw));
  if (!inserted) {
    return utils::Status::AlreadyExists("fh " + std::to_string(fh) +
                                        " is already open");
  }
  return utils::Status::OK();
}

std::shared_ptr<FileReadWriter> FileHandleManager::Find(uint64_t fh) {
  std::shared_lock lock(mutex_);
  auto it = files_.find(fh);
  return it != files_.end() ? it->second : nullptr;
}

void FileHandleManager::Release(uint64_t fh) {
  std::shared_ptr<FileReadWriter> rw;
  {
    std::unique_lock lock(mutex_);
    auto it = files_.find(fh);
    if (it == files_.end()) return;
    rw = std::move(it->second);
    files_.erase(it);
  }
  // Flush outside the lock — I/O can take a long time.
  if (rw) rw->Flush();
}

}  // namespace swordfs::vfs
