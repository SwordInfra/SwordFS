// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaTxn.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <folly/fibers/FiberManagerInternal.h>

#include <algorithm>

#include "metadata/Types.hpp"
#include "metadata/Utils.hpp"
#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"

using Status = swordfs::utils::Status;

namespace swordfs::metadata {

// ────────────────────────────────────────────────────────────────
// Transaction primitives.  Every method runs with the store lock
// held; the transaction is the Transact() callback's lifetime.
// Reads return snapshot copies; writes are by-ino.
// ────────────────────────────────────────────────────────────────

Status MemMetaTxn::LookupInode(InodeID ino, SwordFsInode *out) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  if (out) {
    *out = *inode;
  }
  return Status::OK();
}

size_t MemMetaTxn::InodeCount() {
  return store_->inodes_.size();
}

Status MemMetaTxn::WriteAttr(InodeID ino, const struct stat *attr) {
  if (!attr) {
    return Status::InvalidArgument("null attr");
  }
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  inode->attr = *attr;
  return Status::OK();
}

Status MemMetaTxn::TouchInode(InodeID ino, SetAttrField fields) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  inode->Touch(fields);
  return Status::OK();
}

Status MemMetaTxn::AdjustNlink(InodeID ino, int delta) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  inode->attr.st_nlink += delta;
  return Status::OK();
}

Status MemMetaTxn::AddNlookup(InodeID ino, int64_t delta) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  if (delta < 0 && static_cast<uint64_t>(-delta) >= inode->nlookup) {
    inode->nlookup = 0;
  } else {
    inode->nlookup += delta;
  }
  return Status::OK();
}

Status MemMetaTxn::SetSymlinkTarget(InodeID ino, std::string_view target) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  inode->symlink_target = target;
  inode->attr.st_size = inode->symlink_target.size();
  return Status::OK();
}

Status MemMetaTxn::LookupEntry(InodeID parent_ino, std::string_view name,
                               SwordFsInode *out) {
  SwordFsInode *parent = FindInode(parent_ino);
  if (!parent) {
    return Status::NotFound("parent directory not found");
  }
  if (!parent->IsDir()) {
    return Status::NotDirectory("parent is not a directory");
  }
  SwordFsInode *inode = FindEntry(parent_ino, name);
  if (!inode) {
    return Status::NotFound("entry not found");
  }
  if (out) {
    *out = *inode;
  }
  return Status::OK();
}

Status MemMetaTxn::AddEntry(InodeID parent_ino, std::string_view name,
                            mode_t mode, uint64_t nlookup,
                            SwordFsInode *out) {
  SwordFsInode *parent = FindInode(parent_ino);
  if (!parent) {
    return Status::NotFound("parent directory not found");
  }
  if (!parent->IsDir()) {
    return Status::NotDirectory("parent is not a directory");
  }
  if (FindEntry(parent_ino, name) != nullptr) {
    return Status::AlreadyExists("entry already exists");
  }

  auto &ctx = folly::fibers::local<swordfs::utils::SwordFsContext>();
  struct stat st = MakeStat(mode, ::time(nullptr));
  st.st_uid = ctx.uid;
  st.st_gid = parent->attr.st_gid;
  st.st_ino = store_->next_ino_.fetch_add(1, std::memory_order_relaxed);

  SwordFsInode *child = new SwordFsInode{st.st_ino, st, parent_ino, nlookup};
  InsertInode(child);
  LinkEntry(parent_ino, name, child);

  if (out) {
    *out = *child;
  }
  return Status::OK();
}

Status MemMetaTxn::MoveEntry(InodeID old_parent_ino,
                             std::string_view old_name,
                             InodeID new_parent_ino,
                             std::string_view new_name) {
  SwordFsInode *old_parent = FindInode(old_parent_ino);
  if (!old_parent) {
    return Status::NotFound("old parent directory not found");
  }
  if (!old_parent->IsDir()) {
    return Status::NotDirectory("old parent is not a directory");
  }
  if (FindEntry(old_parent_ino, old_name) == nullptr) {
    return Status::NotFound("source entry not found");
  }

  SwordFsInode *new_parent = FindInode(new_parent_ino);
  if (!new_parent) {
    return Status::NotFound("new parent directory not found");
  }
  if (!new_parent->IsDir()) {
    return Status::NotDirectory("new parent is not a directory");
  }
  if (FindEntry(new_parent_ino, new_name) != nullptr) {
    return Status::AlreadyExists("target entry already exists");
  }

  SwordFsInode *child = UnlinkEntry(old_parent_ino, old_name);
  if (!child) {
    return Status::Internal("unlink entry failed");
  }
  child->parent_ino = new_parent_ino;
  LinkEntry(new_parent_ino, new_name, child);
  return Status::OK();
}

Status MemMetaTxn::Unlink(InodeID parent_ino, std::string_view name,
                          nlink_t *post_nlink) {
  SwordFsInode *child = FindEntry(parent_ino, name);
  if (!child) {
    // Idempotent: a parallel unlink already removed the entry.
    if (post_nlink) {
      *post_nlink = 0;
    }
    return Status::OK();
  }

  if (child->IsDir() && !IsDirEmpty(child->ino)) {
    return Status::NotEmpty("directory not empty");
  }

  // Remove the directory entry. Decrement nlink to track the hard-link
  // count. The inode (and its chunks) survive here; the caller decides
  // whether to follow up with `ReclaimData` once it has confirmed no
  // open file descriptor still references the inode.
  UnlinkEntry(parent_ino, name);

  if (child->IsDir()) {
    // Directories cannot be hard-linked; always reclaim immediately.
    DeleteInode(child->ino);
    if (post_nlink) {
      // Inode is gone — surface 0 so the caller doesn't try to read
      // further metadata for it.
      *post_nlink = 0;
    }
    return Status::OK();
  }

  // File: decrement nlink only. If no names remain, the inode stays
  // alive until the caller (VfsImpl::Unlink or InodeHandle::Close)
  // confirms no fd is open and calls ReclaimData.
  child->attr.st_nlink--;
  if (post_nlink) {
    // Read it back inside the same transaction so the caller sees the
    // exact post-decrement value with no chance of a concurrent Link
    // racing in between.
    *post_nlink = child->attr.st_nlink;
  }
  return Status::OK();
}

Status MemMetaTxn::LinkExistingEntry(InodeID parent_ino,
                                     std::string_view name, InodeID ino) {
  SwordFsInode *parent = FindInode(parent_ino);
  if (!parent) {
    return Status::NotFound("parent directory not found");
  }
  if (!parent->IsDir()) {
    return Status::NotDirectory("parent is not a directory");
  }
  if (FindEntry(parent_ino, name) != nullptr) {
    return Status::AlreadyExists("entry already exists");
  }
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }

  // NOTE: full POSIX hard-link semantics (parent tracking for "..",
  // nlink accounting for the last reference) are not yet wired here.
  // Future work: introduce an internal "pointer inode" record so that
  // link(2) and unlink(2) keep the target's nlink in sync and resolve
  // link attributes transparently.
  inode->attr.st_nlink++;
  LinkEntry(parent_ino, name, inode);
  return Status::OK();
}

Status MemMetaTxn::ListEntries(InodeID ino,
                               std::vector<SwordFsEntry> *entries) {
  SwordFsInode *dir = FindInode(ino);
  if (dir == nullptr) {
    return Status::NotFound("directory not found");
  }
  if (!dir->IsDir()) {
    return Status::NotDirectory("not a directory");
  }

  entries->push_back({".", DT_DIR, ino});
  entries->push_back({"..", DT_DIR, dir->parent_ino});

  auto dir_it = store_->dirs_.find(ino);
  if (dir_it != store_->dirs_.end()) {
    for (const auto &[name, child] : dir_it->second) {
      entries->push_back({name, ModeToDt(child->attr.st_mode), child->ino});
    }
  }
  return Status::OK();
}

bool MemMetaTxn::IsDescendantOf(InodeID ancestor_ino,
                                InodeID child_ino) const {
  // Defence in depth: a corrupted tree (e.g. a directory cycle) must not
  // send this DFS into an infinite loop, so track visited inodes.
  folly::F14FastSet<InodeID> visited;
  std::vector<InodeID> stack;
  stack.push_back(ancestor_ino);

  while (!stack.empty()) {
    InodeID ino = stack.back();
    stack.pop_back();
    if (!visited.insert(ino).second) {
      continue;
    }

    auto it = store_->dirs_.find(ino);
    if (it == store_->dirs_.end()) {
      continue;
    }
    for (const auto &[_, child] : it->second) {
      if (child->ino == child_ino) {
        return true;
      }
      if (child->IsDir()) {
        stack.push_back(child->ino);
      }
    }
  }
  return false;
}

Status MemMetaTxn::SwapEntries(InodeID parent_a_ino, std::string_view name_a,
                               InodeID parent_b_ino,
                               std::string_view name_b) {
  auto dir_a_it = store_->dirs_.find(parent_a_ino);
  if (dir_a_it == store_->dirs_.end()) {
    return Status::NotFound("parent A directory not found");
  }
  auto it_a = dir_a_it->second.find(name_a);
  if (it_a == dir_a_it->second.end()) {
    return Status::NotFound("source entry A not found");
  }

  auto dir_b_it = store_->dirs_.find(parent_b_ino);
  if (dir_b_it == store_->dirs_.end()) {
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
  SwordFsInode *inode_a = it_a->second;
  SwordFsInode *inode_b = it_b->second;
  dir_a_it->second[std::string(name_a)] = inode_b;
  dir_b_it->second[std::string(name_b)] = inode_a;

  // Keep parent_ino in sync with the new locations so the synthetic ".."
  // entries produced by ListEntries point at the right parent.  (For a
  // same-directory swap both parents are identical, so this is a no-op;
  // for same-entry swaps inode_a == inode_b and the values match too.)
  inode_a->parent_ino = parent_b_ino;
  inode_b->parent_ino = parent_a_ino;

  return Status::OK();
}

Status MemMetaTxn::AddChunk(InodeID ino, const ChunkMeta &cm) {
  auto &chunk_map = store_->chunks_[ino];
  if (chunk_map.count(cm.index) > 0) {
    return Status::AlreadyExists(
        "chunk already exists at index " + std::to_string(cm.index));
  }
  chunk_map[cm.index] = cm;
  return Status::OK();
}

Status MemMetaTxn::FindChunk(InodeID ino, ChunkIndex idx, ChunkMeta *cm) {
  auto ino_it = store_->chunks_.find(ino);
  if (ino_it == store_->chunks_.end()) {
    return Status::NotFound("no chunks for inode " + std::to_string(ino));
  }
  auto chunk_it = ino_it->second.find(idx);
  if (chunk_it == ino_it->second.end()) {
    return Status::NotFound("chunk not found at index " + std::to_string(idx));
  }
  const auto &c = chunk_it->second;
  if (c.index != idx) {
    return Status::NotFound("chunk index mismatch");
  }
  if (cm) {
    *cm = c;
  }
  return Status::OK();
}

Status MemMetaTxn::TruncateChunks(InodeID ino, size_t new_size) {
  auto ino_it = store_->chunks_.find(ino);
  if (ino_it == store_->chunks_.end()) {
    return Status::OK();
  }
  if (new_size == 0) {
    store_->chunks_.erase(ino_it);
    return Status::OK();
  }

  auto &cmap = ino_it->second;
  for (auto cit = cmap.begin(); cit != cmap.end();) {
    const auto &cm = cit->second;
    if (cm.start_offset >= new_size) {
      // Chunk lies entirely beyond the new size — drop it.
      cit = cmap.erase(cit);
      continue;
    }
    // Chunk straddles the new size — clamp its size.
    uint64_t new_chunk_size = new_size - cm.start_offset;
    if (cm.size > new_chunk_size) {
      cit->second.size = static_cast<size_t>(new_chunk_size);
    }
    ++cit;
  }
  return Status::OK();
}

Status MemMetaTxn::ReclaimInode(InodeID ino) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::OK();  // already reclaimed
  }
  if (inode->attr.st_nlink == 0) {
    DeleteInode(ino);
  }
  return Status::OK();
}

Status MemMetaTxn::ListChunks(InodeID ino, std::vector<ChunkMeta> *out) {
  if (!out) {
    return Status::InvalidArgument("null out");
  }
  out->clear();
  auto it = store_->chunks_.find(ino);
  if (it == store_->chunks_.end()) {
    return Status::OK();
  }
  out->reserve(it->second.size());
  for (const auto &[idx, cm] : it->second) {
    out->push_back(cm);
  }
  // F14FastMap iteration order is unspecified; the contract for
  // ListChunks is ascending ChunkIndex so a single audit log of
  // deletes reads top-to-bottom. Sort by index to honour it.
  std::sort(out->begin(), out->end(),
            [](const ChunkMeta &a, const ChunkMeta &b) {
              return a.index < b.index;
            });
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Private helpers — direct accessors over the store's tables.  No
// "Locked" suffix: every MemMetaTxn method runs inside the store's
// critical section by construction.
// ────────────────────────────────────────────────────────────────

SwordFsInode *MemMetaTxn::FindInode(InodeID ino) {
  auto it = store_->inodes_.find(ino);
  return it != store_->inodes_.end() ? it->second : nullptr;
}

void MemMetaTxn::InsertInode(SwordFsInode *inode) {
  store_->inodes_[inode->ino] = inode;
  if (S_ISDIR(inode->attr.st_mode)) {
    store_->dirs_.try_emplace(inode->ino);
  }
}

void MemMetaTxn::DeleteInode(InodeID ino) {
  auto it = store_->inodes_.find(ino);
  if (it != store_->inodes_.end()) {
    delete it->second;
    store_->inodes_.erase(it);
    store_->dirs_.erase(ino);
    store_->chunks_.erase(ino);
  }
}

SwordFsInode *MemMetaTxn::FindEntry(InodeID parent_ino,
                                    std::string_view name) {
  auto dir_it = store_->dirs_.find(parent_ino);
  if (dir_it == store_->dirs_.end()) {
    return nullptr;
  }
  auto it = dir_it->second.find(name);
  return it != dir_it->second.end() ? it->second : nullptr;
}

void MemMetaTxn::LinkEntry(InodeID parent_ino, std::string_view name,
                           SwordFsInode *inode) {
  store_->dirs_[parent_ino][std::string(name)] = inode;
}

SwordFsInode *MemMetaTxn::UnlinkEntry(InodeID parent_ino,
                                      std::string_view name) {
  auto dir_it = store_->dirs_.find(parent_ino);
  if (dir_it == store_->dirs_.end()) {
    return nullptr;
  }
  auto it = dir_it->second.find(name);
  if (it == dir_it->second.end()) {
    return nullptr;
  }
  SwordFsInode *inode = it->second;
  dir_it->second.erase(it);
  return inode;
}

bool MemMetaTxn::IsDirEmpty(InodeID ino) {
  auto it = store_->dirs_.find(ino);
  CHECK(it != store_->dirs_.end())
      << "IsDirEmpty called for non-directory ino=" << ino;
  return it->second.empty();
}

}  // namespace swordfs::metadata
