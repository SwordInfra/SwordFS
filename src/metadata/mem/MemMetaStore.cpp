// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaStore.hpp"

#include <ctime>
#include <mutex>

#include "metadata/Utils.hpp"

namespace swordfs::metadata {

MemMetaStore::MemMetaStore() : next_ino_(kRootInodeId + 1) {
  std::lock_guard<std::mutex> lock(mutex_);
  time_t now = ::time(nullptr);
  struct stat root_st = MakeStat(S_IFDIR | 0755, now);
  root_st.st_ino = kRootInodeId;
  inodes_[kRootInodeId] =
      new SwordFsInode{kRootInodeId, root_st, kRootInodeId};
  dirs_[kRootInodeId] = {};
}

MemMetaStore::~MemMetaStore() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &[ino, ptr] : inodes_) {
    delete ptr;
  }
}

}  // namespace swordfs::metadata
