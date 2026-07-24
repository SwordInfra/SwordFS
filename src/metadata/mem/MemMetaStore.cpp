// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/Types.hpp"
#include "utils/Logging.hpp"
#define FUSE_USE_VERSION 312
#include <folly/fibers/FiberManagerInternal.h>
#include <fuse_lowlevel.h>

#include "metadata/Utils.hpp"
#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Context.hpp"

using Status = swordfs::utils::Status;

namespace swordfs::metadata {

MemMetaStore::MemMetaStore() : next_ino_(FUSE_ROOT_ID + 1) {
  std::lock_guard<std::mutex> lock(mutex_);
  time_t now = ::time(nullptr);
  struct stat root_st = MakeStat(S_IFDIR | 0755, now);
  root_st.st_ino = FUSE_ROOT_ID;
  inodes_[FUSE_ROOT_ID] = new SwordFsInode{FUSE_ROOT_ID, root_st, 0};
  dirs_[FUSE_ROOT_ID] = {};
}

MemMetaStore::~MemMetaStore() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [ino, ptr] : inodes_) {
    delete ptr;
  }
}

// ────────────────────────────────────────────────────────────────
// Public API
// ────────────────────────────────────────────────────────────────

Status MemMetaStore::LookupInode(InodeID ino, SwordFsInode** out) {
  std::lock_guard<std::mutex> lock(mutex_);
  SwordFsInode* inode = FindInodeLocked(ino);
  if (!inode) return Status::NotFound("inode not found");
  if (out) *out = inode;
  return Status::OK();
}

size_t MemMetaStore::InodeCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return inodes_.size();
}

Status MemMetaStore::LookupEntry(InodeID parent_ino, std::string_view name,
                                 SwordFsInode** out) {
  std::lock_guard<std::mutex> lock(mutex_);
  SwordFsInode* parent = FindInodeLocked(parent_ino);
  if (!parent) return Status::NotFound("parent directory not found");
  if (!parent->IsDir()) return Status::NotDirectory("parent is not a directory");
  SwordFsInode* inode = FindEntryLocked(parent_ino, name);
  if (!inode) return Status::NotFound("entry not found");
  if (out) *out = inode;
  return Status::OK();
}

Status MemMetaStore::AddEntry(InodeID parent_ino, std::string_view name,
                              mode_t mode, uint64_t nlookup,
                              SwordFsInode** out) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* parent = FindInodeLocked(parent_ino);
  if (!parent) return Status::NotFound("parent directory not found");
  if (!parent->IsDir()) return Status::NotDirectory("parent is not a directory");
  if (FindEntryLocked(parent_ino, name) != nullptr)
    return Status::AlreadyExists("entry already exists");

  auto& ctx = folly::fibers::local<swordfs::utils::SwordFsContext>();
  struct stat st = MakeStat(mode, ::time(nullptr));
  st.st_uid = ctx.uid;
  st.st_gid = parent->attr.st_gid;
  st.st_ino = next_ino_.fetch_add(1, std::memory_order_relaxed);

  SwordFsInode* child = new SwordFsInode{st.st_ino, st, nlookup};
  InsertInodeLocked(child);
  LinkEntryLocked(parent_ino, name, child);

  if (out) *out = child;
  return Status::OK();
}

Status MemMetaStore::MoveEntry(InodeID old_parent_ino,
                               std::string_view old_name,
                               InodeID new_parent_ino,
                               std::string_view new_name) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* old_parent = FindInodeLocked(old_parent_ino);
  if (!old_parent) return Status::NotFound("old parent directory not found");
  if (!old_parent->IsDir())
    return Status::NotDirectory("old parent is not a directory");
  if (FindEntryLocked(old_parent_ino, old_name) == nullptr)
    return Status::NotFound("source entry not found");

  SwordFsInode* new_parent = FindInodeLocked(new_parent_ino);
  if (!new_parent) return Status::NotFound("new parent directory not found");
  if (!new_parent->IsDir())
    return Status::NotDirectory("new parent is not a directory");
  if (FindEntryLocked(new_parent_ino, new_name) != nullptr)
    return Status::AlreadyExists("target entry already exists");

  SwordFsInode* child = UnlinkEntryLocked(old_parent_ino, old_name);
  if (!child) return Status::Internal("unlink entry failed");
  LinkEntryLocked(new_parent_ino, new_name, child);
  return Status::OK();
}

Status MemMetaStore::RemoveEntry(InodeID parent_ino, std::string_view name) {
  std::lock_guard<std::mutex> lock(mutex_);

  SwordFsInode* child = FindEntryLocked(parent_ino, name);
  if (!child) return Status::OK();  // idempotent

  if (child->IsDir() && !IsDirEmptyLocked(child->ino))
    return Status::Busy("directory not empty");

  UnlinkEntryLocked(parent_ino, name);
  DeleteInodeLocked(child->ino);
  return Status::OK();
}

Status MemMetaStore::ListEntries(
    InodeID ino,
    std::vector<std::pair<std::string, SwordFsInode*>>* entries) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (FindInodeLocked(ino) == nullptr)
    return Status::NotFound("directory not found");

  auto dir_it = dirs_.find(ino);
  if (dir_it != dirs_.end()) {
    for (const auto& [name, child] : dir_it->second) {
      entries->push_back({name, child});
    }
  }
  return Status::OK();
}

bool MemMetaStore::IsDescendantOf(InodeID ancestor_ino,
                                  InodeID child_ino) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return IsDescendantOfImplLocked(ancestor_ino, child_ino);
}

Status MemMetaStore::SwapEntries(InodeID parent_a_ino, std::string_view name_a,
                                 InodeID parent_b_ino, std::string_view name_b) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto dir_a_it = dirs_.find(parent_a_ino);
  if (dir_a_it == dirs_.end()) {
    return Status::NotFound("parent A directory not found");
  }
  auto it_a = dir_a_it->second.find(name_a);
  if (it_a == dir_a_it->second.end()) {
    return Status::NotFound("source entry A not found");
  }

  auto dir_b_it = dirs_.find(parent_b_ino);
  if (dir_b_it == dirs_.end()) {
    return Status::NotFound("parent B directory not found");
  }
  auto it_b = dir_b_it->second.find(name_b);
  if (it_b == dir_b_it->second.end()) {
    return Status::NotFound("source entry B not found");
  }

  // Atomically swap the inode pointers.  The two-step assignment handles
  // both same-directory and cross-directory swaps correctly:
  //   - Cross-directory: each parent's entry table gets the other's inode.
  //   - Same-directory (different names): values are swapped.
  //   - Same-directory (same name): no-op (identical values).
  SwordFsInode* inode_a = it_a->second;
  SwordFsInode* inode_b = it_b->second;
  dir_a_it->second[std::string(name_a)] = inode_b;
  dir_b_it->second[std::string(name_b)] = inode_a;

  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Private helpers — caller MUST hold mutex_
// ────────────────────────────────────────────────────────────────

SwordFsInode* MemMetaStore::FindInodeLocked(InodeID ino) {
  auto it = inodes_.find(ino);
  return it != inodes_.end() ? it->second : nullptr;
}

void MemMetaStore::InsertInodeLocked(SwordFsInode* inode) {
  inodes_[inode->ino] = inode;
  if (S_ISDIR(inode->attr.st_mode)) {
    dirs_.try_emplace(inode->ino);
  }
}

void MemMetaStore::DeleteInodeLocked(InodeID ino) {
  auto it = inodes_.find(ino);
  if (it != inodes_.end()) {
    delete it->second;
    inodes_.erase(it);
    dirs_.erase(ino);
  }
}

SwordFsInode* MemMetaStore::FindEntryLocked(InodeID parent_ino,
                                            std::string_view name) {
  auto dir_it = dirs_.find(parent_ino);
  if (dir_it == dirs_.end()) return nullptr;
  auto it = dir_it->second.find(name);
  return it != dir_it->second.end() ? it->second : nullptr;
}

void MemMetaStore::LinkEntryLocked(InodeID parent_ino, std::string_view name,
                                   SwordFsInode* inode) {
  dirs_[parent_ino][std::string(name)] = inode;
}

SwordFsInode* MemMetaStore::UnlinkEntryLocked(InodeID parent_ino,
                                              std::string_view name) {
  auto dir_it = dirs_.find(parent_ino);
  if (dir_it == dirs_.end()) return nullptr;
  auto it = dir_it->second.find(name);
  if (it == dir_it->second.end()) return nullptr;
  SwordFsInode* inode = it->second;
  dir_it->second.erase(it);
  return inode;
}

bool MemMetaStore::IsDirEmptyLocked(InodeID ino) {
  auto it = dirs_.find(ino);
  CHECK(it != dirs_.end())
      << "IsDirEmptyLocked called for non-directory ino=" << ino;
  return it->second.empty();
}

bool MemMetaStore::IsDescendantOfImplLocked(InodeID current_ino,
                                            InodeID target_ino) const {
  // Caller must hold mutex_.

  std::vector<InodeID> stack;
  stack.push_back(current_ino);

  while (!stack.empty()) {
    InodeID ino = stack.back();
    stack.pop_back();

    auto it = dirs_.find(ino);
    if (it == dirs_.end()) continue;
    for (const auto& [_, child] : it->second) {
      if (child->ino == target_ino) return true;
      if (child->IsDir()) {
        stack.push_back(child->ino);
      }
    }
  }
  return false;
}

// ────────────────────────────────────────────────────────────────
// Chunk Slice storage (S3 Phase 2)
// ────────────────────────────────────────────────────────────────

Status MemMetaStore::AppendSlice(InodeID ino,
                                  const swordfs::storage::Slice& slice) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& list = chunks_[ino];
  list.slices.push_back(slice);
  // Keep slices sorted by offset for efficient Read stitching.
  std::sort(list.slices.begin(), list.slices.end());
  return Status::OK();
}

Status MemMetaStore::GetSlices(InodeID ino,
                                swordfs::storage::SliceList* out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = chunks_.find(ino);
  if (it == chunks_.end()) {
    *out = swordfs::storage::SliceList{};
    return Status::OK();
  }
  *out = it->second;
  return Status::OK();
}

uint64_t MemMetaStore::NextSliceID(InodeID ino) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& list = chunks_[ino];
  return list.AllocID();
}

}  // namespace swordfs::metadata
