// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaTxn.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <folly/fibers/FiberManagerInternal.h>

#include <algorithm>
#include <limits>
#include <utility>

#include "chunk/ChunkObjectKey.hpp"
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
//
// Primitives maintain the tree's STRUCTURAL INVARIANTS themselves:
//   - creating/removing a subdirectory entry adjusts the parent's
//     nlink (the child's ".." backlink);
//   - moving an entry across parents adjusts both parents' nlink;
//   - any entry-list change bumps the parent directories' mtime/ctime;
//   - re-linking an inode (move/swap/link) bumps its ctime.
// Callers compose primitives for POLICY (permissions, POSIX error
// codes, flag dispatch) and never repeat this bookkeeping.
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

uint64_t MemMetaTxn::InodeCount() {
  return store_->inodes_.size();
}

Status MemMetaTxn::AllocateChunkRevision(ChunkRevision *revision) {
  if (revision == nullptr) {
    return Status::InvalidArgument("chunk revision output is null");
  }
  *revision = store_->next_chunk_revision_++;
  return Status::OK();
}

Status MemMetaTxn::SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }

  SwordFsAttr st = inode->attr;
  const SwordFsAttr &requested = attr;
  bool owner_changed = false;
  bool size_changed = false;

  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    st.mode = (st.mode & S_IFMT) | (requested.mode & 07777);
  }
  if (HasSetAttrField(fields, SetAttrField::kUid)) {
    owner_changed = owner_changed || st.uid != requested.uid;
    st.uid = requested.uid;
  }
  if (HasSetAttrField(fields, SetAttrField::kGid)) {
    owner_changed = owner_changed || st.gid != requested.gid;
    st.gid = requested.gid;
  }
  if (HasSetAttrField(fields, SetAttrField::kSize)) {
    size_changed = st.size != requested.size;
    st.size = requested.size;
  }
  if (HasSetAttrField(fields, SetAttrField::kAtime)) {
    st.atime = requested.atime;
    st.atime_nsec = requested.atime_nsec;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtime)) {
    st.mtime = requested.mtime;
    st.mtime_nsec = requested.mtime_nsec;
  }
  if (HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
    st.atime = static_cast<int64_t>(::time(nullptr));
    st.atime_nsec = 0;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
    st.mtime = static_cast<int64_t>(::time(nullptr));
    st.mtime_nsec = 0;
  }
  if (HasSetAttrField(fields, SetAttrField::kCtime)) {
    st.ctime = requested.ctime;
    st.ctime_nsec = requested.ctime_nsec;
  }

  if (size_changed || owner_changed) {
    st.KillSUID();
  }
  if (size_changed && !HasSetAttrField(fields, SetAttrField::kMtime) &&
      !HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
    st.mtime = static_cast<int64_t>(::time(nullptr));
    st.mtime_nsec = 0;
  }
  if (!HasSetAttrField(fields, SetAttrField::kCtime)) {
    st.ctime = static_cast<int64_t>(::time(nullptr));
    st.ctime_nsec = 0;
  }

  Status status = WriteAttr(ino, st);
  if (!status.ok()) {
    return status;
  }
  if (size_changed) {
    status = TruncateChunks(ino, st.size);
    if (!status.ok()) {
      return status;
    }
  }
  if (out) {
    *out = *inode;
  }
  return Status::OK();
}

Status MemMetaTxn::Truncate(InodeID ino, uint64_t size) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  if (inode->attr.size == static_cast<int64_t>(size)) {
    return Status::OK();
  }

  SwordFsAttr st = inode->attr;
  st.size = size;
  st.KillSUID();
  st.mtime = static_cast<int64_t>(::time(nullptr));
  st.mtime_nsec = 0;
  st.ctime = static_cast<int64_t>(::time(nullptr));
  st.ctime_nsec = 0;

  Status status = WriteAttr(ino, st);
  if (!status.ok()) {
    return status;
  }
  return TruncateChunks(ino, size);
}

Status MemMetaTxn::WriteAttr(InodeID ino, const SwordFsAttr &attr) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  inode->attr = attr;
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
  inode->attr.nlink += delta;
  return Status::OK();
}

Status MemMetaTxn::SetSymlinkTarget(InodeID ino, std::string_view target) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  inode->symlink_target = target;
  inode->attr.size = inode->symlink_target.size();
  return Status::OK();
}

Status MemMetaTxn::LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
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

Status MemMetaTxn::AddEntry(InodeID parent_ino, std::string_view name, uint32_t mode, SwordFsInode *out) {
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
  SwordFsAttr attr(store_->next_ino_.fetch_add(1, std::memory_order_relaxed), static_cast<uint32_t>(mode), ctx.uid,
                   parent->attr.gid);

  auto child = std::make_unique<SwordFsInode>(attr.ino, attr, parent_ino);
  SwordFsInode *child_ptr = child.get();
  InsertInode(std::move(child));
  LinkEntry(parent_ino, name, child_ptr);

  // A new subdirectory's ".." is an additional hard link to the parent,
  // and the entry-list change bumps the parent's mtime/ctime.
  if (child_ptr->IsDir()) {
    parent->attr.nlink++;
  }
  parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);

  if (out) {
    *out = *child_ptr;
  }
  return Status::OK();
}

Status MemMetaTxn::MoveEntry(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                             std::string_view new_name, bool overwrite) {
  SwordFsInode *old_parent = FindInode(old_parent_ino);
  if (!old_parent) {
    return Status::NotFound("old parent directory not found");
  }
  if (!old_parent->IsDir()) {
    return Status::NotDirectory("old parent is not a directory");
  }
  SwordFsInode *new_parent = FindInode(new_parent_ino);
  if (!new_parent) {
    return Status::NotFound("new parent directory not found");
  }
  if (!new_parent->IsDir()) {
    return Status::NotDirectory("new parent is not a directory");
  }

  SwordFsInode *child = FindEntry(old_parent_ino, old_name);
  if (!child) {
    return Status::NotFound("source entry not found");
  }

  // A directory can never be moved into itself or its own subtree —
  // that would create a cycle.  (The descendant check alone misses the
  // direct self-move new_parent_ino == child->ino.)
  if (child->IsDir() && (new_parent_ino == child->ino || IsDescendantOf(child->ino, new_parent_ino))) {
    return Status::InvalidArgument("cannot move directory into itself");
  }

  if (SwordFsInode *victim = FindEntry(new_parent_ino, new_name)) {
    if (!overwrite) {
      return Status::AlreadyExists("target entry already exists");
    }
    // Rename onto itself — the same inode, possibly through another
    // hard link — is a no-op.
    if (victim == child) {
      return Status::OK();
    }
    // Cannot replace a directory with a non-directory or vice versa.
    if (victim->IsDir() != child->IsDir()) {
      if (victim->IsDir()) {
        return Status::IsDirectory("target is a directory");
      }
      return Status::NotDirectory("target is not a directory");
    }
    // Unlink detaches the victim. Empty directories are reclaimed by
    // Unlink itself (which frees the inode record, so the victim pointer must
    // not be dereferenced afterwards); file inodes survive when their last
    // name disappears so the background Reclaimer can apply the local open-fd
    // fence before crossing the metadata point of no return.
    Status status = Unlink(new_parent_ino, new_name);
    if (!status.ok()) {
      return status;
    }
  }

  UnlinkEntry(old_parent_ino, old_name);
  child->parent_ino = new_parent_ino;
  LinkEntry(new_parent_ino, new_name, child);

  // Moving a directory re-parents its "..": the old parent loses a hard
  // link and the new parent gains one.  (Same-parent moves change no
  // nlink.)
  if (child->IsDir() && old_parent_ino != new_parent_ino) {
    old_parent->attr.nlink--;
    new_parent->attr.nlink++;
  }
  // Both entry lists changed; the re-linked inode's ctime bumps.
  old_parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  new_parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  child->Touch(SetAttrField::kCtime);
  return Status::OK();
}

Status MemMetaTxn::Unlink(InodeID parent_ino, std::string_view name) {
  SwordFsInode *child = FindEntry(parent_ino, name);
  if (!child) {
    return Status::NotFound("entry not found");
  }

  if (child->IsDir() && !IsDirEmpty(child->ino)) {
    return Status::NotEmpty("directory not empty");
  }

  SwordFsInode *parent = FindInode(parent_ino);

  // Remove the directory entry. Decrement nlink to track the hard-link count.
  // The inode (and its chunks) survive here; durable orphan state tells the
  // background Reclaimer when it may later attempt open-fd-aware preparation.
  UnlinkEntry(parent_ino, name);
  parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);

  if (child->IsDir()) {
    // The removed subdirectory's ".." no longer points back at the
    // parent, so the parent loses a hard link.  Directories cannot be
    // hard-linked; always reclaim immediately.
    parent->attr.nlink--;
    DeleteInode(child->ino);
    return Status::OK();
  }

  // File: decrement nlink only. If no names remain, the inode stays alive
  // until the background Reclaimer confirms no local fd is open and prepares
  // the durable reclaim.
  child->attr.nlink--;
  if (child->attr.nlink == 0) {
    // Last name gone: publish the inode as an orphan candidate in this same
    // transaction, so the fact that it is unreachable survives a crash
    // before the caller gets to reclaim it. The candidate is what mount-time
    // reconciliation promotes; it carries no claim on the inode's data.
    store_->orphans_.insert(child->ino);
  }
  return Status::OK();
}

Status MemMetaTxn::LinkExistingEntry(InodeID parent_ino, std::string_view name, InodeID ino, SwordFsInode *out) {
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

  // A revived orphan candidate (nlink 0 -> 1) is no longer an orphan; drop
  // the marker in the same transaction that re-links it so reconciliation
  // can never reclaim an inode that has a name again.
  if (inode->attr.nlink == 0) {
    store_->orphans_.erase(ino);
  }

  inode->attr.nlink++;
  LinkEntry(parent_ino, name, inode);
  inode->Touch(SetAttrField::kCtime);
  parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  if (out) {
    *out = *inode;
  }
  return Status::OK();
}

Status MemMetaTxn::ListEntries(InodeID ino, std::vector<SwordFsEntry> *entries) {
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
      entries->push_back({name, ModeToDt(child->attr.mode), child->ino});
    }
  }
  return Status::OK();
}

bool MemMetaTxn::IsDescendantOf(InodeID ancestor_ino, InodeID child_ino) const {
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

Status MemMetaTxn::SwapEntries(InodeID parent_a_ino, std::string_view name_a, InodeID parent_b_ino,
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

  SwordFsInode *inode_a = it_a->second;
  SwordFsInode *inode_b = it_b->second;

  // Neither directory may end up inside its own subtree — a swap that
  // places a directory beneath itself would create a cycle.  Both
  // directions must be checked: a swap moves A under parent_b AND B
  // under parent_a.
  if (inode_a->IsDir() && (parent_b_ino == inode_a->ino || IsDescendantOf(inode_a->ino, parent_b_ino))) {
    return Status::InvalidArgument("cannot move directory into itself");
  }
  if (inode_b->IsDir() && (parent_a_ino == inode_b->ino || IsDescendantOf(inode_b->ino, parent_a_ino))) {
    return Status::InvalidArgument("cannot move directory into itself");
  }

  // Atomically swap the inode pointers.  The two-step assignment handles
  // both same-directory and cross-directory swaps correctly:
  //   - Cross-directory: each parent's entry table gets the other's inode.
  //   - Same-directory (different names): values are swapped.
  //   - Same-directory (same name): no-op (identical values).
  dir_a_it->second[std::string(name_a)] = inode_b;
  dir_b_it->second[std::string(name_b)] = inode_a;

  // Keep parent_ino in sync with the new locations so the synthetic ".."
  // entries produced by ListEntries point at the right parent.  (For a
  // same-directory swap both parents are identical, so this is a no-op;
  // for same-entry swaps inode_a == inode_b and the values match too.)
  // No nlink adjustment is needed: each parent loses one entry and gains
  // one.
  inode_a->parent_ino = parent_b_ino;
  inode_b->parent_ino = parent_a_ino;

  // Both entries were re-linked: bump ctime on the inodes and
  // mtime/ctime on the parent directories.
  inode_a->Touch(SetAttrField::kCtime);
  inode_b->Touch(SetAttrField::kCtime);
  FindInode(parent_a_ino)->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  FindInode(parent_b_ino)->Touch(SetAttrField::kMtime | SetAttrField::kCtime);

  return Status::OK();
}

Status MemMetaTxn::CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                               const SwordFsChunk &replacement) {
  if (replacement.revision == kInvalidChunkRevision ||
      (expected.has_value() && expected->revision == kInvalidChunkRevision)) {
    return Status::InvalidArgument("chunk revision is invalid");
  }
  if (expected.has_value() && replacement.revision <= expected->revision) {
    return Status::InvalidArgument("replacement revision must increase");
  }
  if (expected.has_value() &&
      (expected->index != replacement.index || expected->start_offset != replacement.start_offset)) {
    return Status::InvalidArgument("replacement must preserve chunk index and start offset");
  }
  auto queue_pending_delete = [&](const SwordFsChunk &chunk) {
    const auto object_key = chunk::FormatChunkObjectKey(ino, chunk.index, chunk.revision);
    store_->pending_deletes_.insert_or_assign(
        object_key, PendingDelete{.ino = ino, .chunk = ReclaimChunk{.descriptor = chunk, .key = object_key}});
  };

  auto *inode = FindInode(ino);
  if (inode == nullptr) {
    queue_pending_delete(replacement);
    return Status::NotFound("inode not found: " + std::to_string(ino));
  }
  if (!inode->IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }
  if (replacement.size > std::numeric_limits<uint64_t>::max() - replacement.start_offset) {
    return Status::InvalidArgument("chunk end offset overflows");
  }

  auto &chunk_map = store_->chunks_[ino];
  auto it = chunk_map.find(replacement.index);
  bool replacement_already_published = false;
  if (it != chunk_map.end()) {
    replacement_already_published = it->second == replacement;
    const bool current_matches_expected = expected.has_value() && it->second == *expected;
    if (!replacement_already_published && !current_matches_expected) {
      queue_pending_delete(replacement);
      return Status::AlreadyExists("chunk changed before publication at index " + std::to_string(replacement.index));
    }
    if (current_matches_expected || (replacement_already_published && expected.has_value())) {
      // Memory transactions are one critical section, so publishing this
      // cleanup candidate and replacing/replaying the descriptor are atomic.
      // Persistent backends may register the candidate after a known metadata
      // outcome because cleanup completeness is not publication correctness.
      queue_pending_delete(*expected);
    }
  } else if (expected.has_value()) {
    queue_pending_delete(replacement);
    return Status::NotFound("chunk not found at index " + std::to_string(replacement.index));
  }

  if (!replacement_already_published) {
    chunk_map[replacement.index] = replacement;
  }

  const uint64_t chunk_end = replacement.start_offset + replacement.size;
  if (chunk_end > inode->attr.size) {
    inode->attr.size = chunk_end;
  }
  inode->attr.KillSUID();
  inode->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return Status::OK();
}

Status MemMetaTxn::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
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
  if (chunk) {
    *chunk = c;
  }
  return Status::OK();
}

Status MemMetaTxn::TruncateChunks(InodeID ino, uint64_t new_size) {
  auto ino_it = store_->chunks_.find(ino);
  if (ino_it == store_->chunks_.end()) {
    return Status::OK();
  }
  auto &cmap = ino_it->second;
  for (auto cit = cmap.begin(); cit != cmap.end();) {
    auto &chunk = cit->second;
    if (chunk.start_offset >= new_size) {
      // The memory backend can record cleanup and drop the descriptor in the
      // same critical section. Persistent backends may register cleanup after
      // the authoritative truncate transaction has a known successful result.
      const auto object_key = chunk::FormatChunkObjectKey(ino, chunk.index, chunk.revision);
      store_->pending_deletes_.insert_or_assign(
          object_key, PendingDelete{.ino = ino, .chunk = ReclaimChunk{.descriptor = chunk, .key = object_key}});
      cit = cmap.erase(cit);
      continue;
    }
    // Chunk straddles the new size — clamp its size.
    uint64_t new_chunk_size = new_size - chunk.start_offset;
    if (chunk.size > new_chunk_size) {
      chunk.size = new_chunk_size;
    }
    ++cit;
  }
  return Status::OK();
}

Status MemMetaTxn::PrepareReclaim(InodeID ino, std::optional<ReclaimWork> *work) {
  if (work == nullptr) {
    return Status::InvalidArgument("reclaim work output is null");
  }
  work->reset();

  // Idempotent replay: once the point of no return has been crossed, the
  // frozen record — not the (already removed) live inode — is the authority,
  // so crash recovery and retries get the same work back unchanged.
  auto pending_it = store_->pending_reclaims_.find(ino);
  if (pending_it != store_->pending_reclaims_.end()) {
    *work = pending_it->second;
    return Status::OK();
  }

  SwordFsInode *inode = FindInode(ino);
  if (inode == nullptr) {
    // Already reclaimed. Any marker left behind is stale.
    store_->orphans_.erase(ino);
    return Status::OK();
  }
  if (inode->IsDir() || inode->attr.nlink != 0) {
    // A concurrent Link revived the inode before this transaction, or the
    // node is a directory (never reclaimed through an orphan candidate).
    // Drop the marker and leave the inode and its objects untouched.
    store_->orphans_.erase(ino);
    return Status::OK();
  }

  ReclaimWork frozen;
  frozen.ino = ino;
  auto chunks_it = store_->chunks_.find(ino);
  if (chunks_it != store_->chunks_.end()) {
    frozen.chunks.reserve(chunks_it->second.size());
    for (const auto &[index, chunk] : chunks_it->second) {
      (void)index;
      // Freeze the object identity that the authoritative descriptor derives
      // right now. Later deletions replay this frozen key, never a key
      // rebuilt from live state.
      frozen.chunks.push_back(ReclaimChunk{chunk, chunk::FormatChunkObjectKey(ino, chunk.index, chunk.revision)});
    }
    std::sort(frozen.chunks.begin(), frozen.chunks.end(),
              [](const ReclaimChunk &a, const ReclaimChunk &b) { return a.descriptor.index < b.descriptor.index; });
  }

  // Retain the frozen work durably, then drop the live inode (and with it the
  // only other copy of the descriptors) and the orphan marker. This is the
  // point of no return: from here on no Link can revive the inode and every
  // remaining step is idempotent.
  store_->pending_reclaims_[ino] = frozen;
  store_->orphans_.erase(ino);
  DeleteInode(ino);

  *work = std::move(frozen);
  return Status::OK();
}

Status MemMetaTxn::CompleteReclaim(InodeID ino) {
  store_->pending_reclaims_.erase(ino);
  return Status::OK();
}

Status MemMetaTxn::ListPendingDeletes(std::vector<PendingDelete> &out) {
  out.clear();
  out.reserve(store_->pending_deletes_.size());
  for (const auto &[key, pending] : store_->pending_deletes_) {
    (void)key;
    out.push_back(pending);
  }
  return Status::OK();
}

Status MemMetaTxn::CompletePendingDelete(std::string_view key) {
  store_->pending_deletes_.erase(std::string(key));
  return Status::OK();
}

Status MemMetaTxn::ListOrphanCandidates(std::vector<InodeID> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("orphan candidate output is null");
  }
  out->clear();
  out->reserve(store_->orphans_.size());
  for (InodeID ino : store_->orphans_) {
    out->push_back(ino);
  }
  // F14 set iteration order is unspecified; sort so a reconciliation pass is
  // deterministic and its logs read in a stable order.
  std::sort(out->begin(), out->end());
  return Status::OK();
}

Status MemMetaTxn::ListPendingReclaims(std::vector<ReclaimWork> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("pending reclaim output is null");
  }
  out->clear();
  out->reserve(store_->pending_reclaims_.size());
  for (const auto &[ino, work] : store_->pending_reclaims_) {
    (void)ino;
    out->push_back(work);
  }
  std::sort(out->begin(), out->end(), [](const ReclaimWork &a, const ReclaimWork &b) { return a.ino < b.ino; });
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Private helpers — direct accessors over the store's tables.  No
// "Locked" suffix: every MemMetaTxn method runs inside the store's
// critical section by construction.
// ────────────────────────────────────────────────────────────────

SwordFsInode *MemMetaTxn::FindInode(InodeID ino) {
  auto it = store_->inodes_.find(ino);
  return it != store_->inodes_.end() ? it->second.get() : nullptr;
}

void MemMetaTxn::InsertInode(std::unique_ptr<SwordFsInode> inode) {
  InodeID ino = inode->ino;
  bool is_dir = S_ISDIR(inode->attr.mode);
  store_->inodes_[ino] = std::move(inode);
  if (is_dir) {
    store_->dirs_.try_emplace(ino);
  }
}

void MemMetaTxn::DeleteInode(InodeID ino) {
  auto it = store_->inodes_.find(ino);
  if (it != store_->inodes_.end()) {
    store_->inodes_.erase(it);
    store_->dirs_.erase(ino);
    store_->chunks_.erase(ino);
  }
}

SwordFsInode *MemMetaTxn::FindEntry(InodeID parent_ino, std::string_view name) {
  auto dir_it = store_->dirs_.find(parent_ino);
  if (dir_it == store_->dirs_.end()) {
    return nullptr;
  }
  auto it = dir_it->second.find(name);
  return it != dir_it->second.end() ? it->second : nullptr;
}

void MemMetaTxn::LinkEntry(InodeID parent_ino, std::string_view name, SwordFsInode *inode) {
  store_->dirs_[parent_ino][std::string(name)] = inode;
}

SwordFsInode *MemMetaTxn::UnlinkEntry(InodeID parent_ino, std::string_view name) {
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
  CHECK(it != store_->dirs_.end()) << "IsDirEmpty called for non-directory ino=" << ino;
  return it->second.empty();
}

}  // namespace swordfs::metadata
