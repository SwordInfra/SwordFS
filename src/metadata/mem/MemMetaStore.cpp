// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/Types.hpp"
#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

#include <folly/fibers/FiberManagerInternal.h>

#include "metadata/mem/MemMetaStore.hpp"
#include "metadata/Utils.hpp"
#include "utils/Context.hpp"

using Status = swordfs::utils::Status;

namespace swordfs::metadata {

MemMetaStore::MemMetaStore() : next_ino_(FUSE_ROOT_ID + 1) {
  time_t now = ::time(nullptr);
  struct stat root_st = MakeStat(S_IFDIR | 0755, now);
  root_st.st_ino = FUSE_ROOT_ID;
  inodes_[FUSE_ROOT_ID] = new SwordFsInode{FUSE_ROOT_ID, root_st, 0};
  dirs_[FUSE_ROOT_ID] = {};
}

MemMetaStore::~MemMetaStore() {
  for (auto& [ino, ptr] : inodes_) {
    delete ptr;
  }
}

// ── Inode operations ──

Status MemMetaStore::LookupInode(InodeID ino, SwordFsInode** out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = inodes_.find(ino);
  if (it == inodes_.end()) {
    return Status::NotFound("inode not found");
  }
  if (out) *out = it->second;
  return Status::OK();
}

size_t MemMetaStore::InodeCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return inodes_.size();
}

// ── Directory operations ──

Status MemMetaStore::LookupEntry(InodeID parent, std::string_view name, SwordFsInode** out) {
  std::lock_guard<std::mutex> lock(mutex_);
  SwordFsInode* inode = FindEntry(parent, name);
  if (!inode) return Status::NotFound("entry not found");
  if (out) *out = inode;
  return Status::OK();
}

Status MemMetaStore::AddEntry(InodeID parent, std::string_view name,
                               mode_t mode, uint64_t nlookup,
                               SwordFsInode** out) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (FindEntry(parent, name) != nullptr) {
    return Status::AlreadyExists("entry already exists");
  }

  auto it = dirs_.find(parent);
  if (it == dirs_.end()) {
    return Status::NotFound("parent directory not found");
  }

  // Derive uid from fiber-local context, gid from parent directory.
  auto& ctx = folly::fibers::local<swordfs::utils::SwordFsContext>();
  uid_t uid = ctx.uid;
  auto parent_it = inodes_.find(parent);
  gid_t gid = parent_it->second->attr.st_gid;

  InodeID ino = next_ino_++;
  struct stat st = MakeStat(mode, ::time(nullptr));
  st.st_ino = ino;
  st.st_uid = uid;
  st.st_gid = gid;
  auto* inode = new SwordFsInode{ino, st, nlookup};
  inodes_[ino] = inode;
  it->second[std::string(name)] = inode;
  if (S_ISDIR(mode)) {
    dirs_.try_emplace(ino);
  }
  if (out) *out = inode;
  return Status::OK();
}

Status MemMetaStore::MoveEntry(InodeID old_parent,
                                std::string_view old_name,
                                InodeID new_parent,
                                std::string_view new_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Remove from old location, saving the inode pointer.
  auto old_it = dirs_.find(old_parent);
  if (old_it == dirs_.end()) {
    return Status::NotFound("old parent directory not found");
  }
  std::string old_key(old_name);
  auto entry_it = old_it->second.find(old_key);
  if (entry_it == old_it->second.end()) {
    return Status::NotFound("source entry not found");
  }
  SwordFsInode* inode = entry_it->second;
  old_it->second.erase(entry_it);

  // Add to new location.
  auto new_it = dirs_.find(new_parent);
  if (new_it == dirs_.end()) {
    return Status::NotFound("new parent directory not found");
  }
  if (FindEntry(new_parent, new_name) != nullptr) {
    return Status::AlreadyExists("target entry already exists");
  }
  new_it->second[std::string(new_name)] = inode;
  return Status::OK();
}

Status MemMetaStore::RemoveEntry(InodeID parent, std::string_view name,
                               bool destroy) {
  std::lock_guard<std::mutex> lock(mutex_);
  SwordFsInode* child = FindEntry(parent, name);
  if (!child) {
    // Parent or entry not found — remove is idempotent.
    return Status::OK();
  }

  if (destroy && child->IsDir()) {
    auto child_dir = dirs_.find(child->ino);
    if (child_dir != dirs_.end() && !child_dir->second.empty()) {
      return Status::Busy("directory not empty");
    }
  }

  // Re-find to erase the entry from the parent directory.
  auto dir_it = dirs_.find(parent);
  dir_it->second.erase(name);

  if (destroy) {
    InodeID child_ino = child->ino;
    if (child->IsDir()) {
      dirs_.erase(child_ino);
    }
    inodes_.erase(child_ino);
    delete child;
  }
  return Status::OK();
}

Status MemMetaStore::ListEntries(InodeID ino, std::vector<std::pair<std::string, SwordFsInode*>>* entries) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = dirs_.find(ino);
  if (it == dirs_.end()) {
    return Status::NotFound("directory not found");
  }
  for (const auto& [name, child] : it->second) {
    entries->push_back({name, child});
  }
  return Status::OK();
}

SwordFsInode* MemMetaStore::FindEntry(InodeID parent, std::string_view name) const {
  auto dir_it = dirs_.find(parent);
  if (dir_it == dirs_.end()) return nullptr;
  auto it = dir_it->second.find(name);
  if (it == dir_it->second.end()) return nullptr;
  return it->second;
}

bool MemMetaStore::IsDescendantOf(InodeID ancestor, InodeID child) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return IsDescendantOfImpl(ancestor, child);
}

bool MemMetaStore::IsDescendantOfImpl(InodeID current,
                                     InodeID target) const {
  auto it = dirs_.find(current);
  if (it == dirs_.end()) return false;
  for (const auto& [_, child_inode] : it->second) {
    if (child_inode->ino == target) return true;
    if (child_inode->IsDir() &&
        IsDescendantOfImpl(child_inode->ino, target)) {
      return true;
    }
  }
  return false;
}

}  // namespace swordfs::metadata
