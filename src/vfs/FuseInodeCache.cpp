// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FuseInodeCache.hpp"

#include <folly/container/F14Map.h>
#include <glog/logging.h>

#include <limits>
#include <mutex>

namespace swordfs::vfs {

namespace {

struct LocalInode {
  metadata::SwordFsInode inode;
  uint64_t lookup_refs = 0;
  bool detached = false;
};

}  // namespace

struct FuseInodeMap : folly::F14FastMap<metadata::InodeID, LocalInode> {};

FuseInodeCache::FuseInodeCache() : inodes_(std::make_unique<FuseInodeMap>()) {
}

FuseInodeCache::~FuseInodeCache() = default;

FuseInodeCache &FuseInodeCache::Instance() {
  static FuseInodeCache instance;
  return instance;
}

void FuseInodeCache::Initialize() {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  inodes_->clear();
}

void FuseInodeCache::RetainLookup(const metadata::SwordFsInode &inode) {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = inodes_->try_emplace(inode.ino, LocalInode{.inode = inode}).first;
  auto &local = it->second;
  CHECK_LT(local.lookup_refs, std::numeric_limits<uint64_t>::max()) << "FUSE lookup reference count overflow";
  ++local.lookup_refs;
  if (!local.detached) {
    local.inode = inode;
  }
}

void FuseInodeCache::RefreshIfRetained(const metadata::SwordFsInode &inode) {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = inodes_->find(inode.ino);
  if (it == inodes_->end() || it->second.detached) {
    return;
  }
  it->second.inode = inode;
}

bool FuseInodeCache::ResolveDetachedAfterNotFound(metadata::InodeID ino, metadata::SwordFsInode *out) {
  CHECK(out != nullptr);
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = inodes_->find(ino);
  if (it == inodes_->end() || it->second.lookup_refs == 0) {
    return false;
  }
  it->second.detached = true;
  it->second.inode.attr.nlink = 0;
  *out = it->second.inode;
  return true;
}

void FuseInodeCache::Forget(metadata::InodeID ino, uint64_t nlookup) {
  if (nlookup == 0) {
    return;
  }
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = inodes_->find(ino);
  if (it == inodes_->end()) {
    return;
  }
  if (nlookup >= it->second.lookup_refs) {
    inodes_->erase(it);
    return;
  }
  it->second.lookup_refs -= nlookup;
}

}  // namespace swordfs::vfs
