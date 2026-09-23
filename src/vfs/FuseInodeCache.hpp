// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <memory>

#include "metadata/Types.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::vfs {

struct FuseInodeMap;

class FuseInodeCache {
 public:
  static FuseInodeCache &Instance();

  void Initialize();
  void RetainLookup(const metadata::SwordFsInode &inode);
  void RefreshIfRetained(const metadata::SwordFsInode &inode);
  bool ResolveDetachedAfterNotFound(metadata::InodeID ino, metadata::SwordFsInode *out);
  void Forget(metadata::InodeID ino, uint64_t nlookup);

 private:
  FuseInodeCache();
  ~FuseInodeCache();

  FuseInodeCache(const FuseInodeCache &) = delete;
  FuseInodeCache &operator=(const FuseInodeCache &) = delete;

 private:
  mutable utils::FiberMutex mutex_;
  std::unique_ptr<FuseInodeMap> inodes_;
};

}  // namespace swordfs::vfs
