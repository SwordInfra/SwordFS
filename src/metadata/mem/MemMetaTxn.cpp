// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemMetaTxn.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <folly/fibers/FiberManagerInternal.h>

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

#include "chunk/internal/ChunkMetadataBridge.hpp"
#include "metadata/ChunkSizePlan.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/InodePolicy.hpp"
#include "metadata/PosixAcl.hpp"
#include "metadata/Types.hpp"
#include "metadata/Utils.hpp"
#include "metadata/mem/MemMetaStore.hpp"
#include "utils/Context.hpp"
#include "utils/Logging.hpp"

using Status = swordfs::utils::Status;

namespace swordfs::metadata {

MemMetaTxn::MemMetaTxn(MemMetaStore *store) : store_(store) {
}

std::string MemMetaTxn::PrivateHash(std::string_view hash) const {
  return ChunkTypeKey(store_->chunk_type_) + "/" + std::string(hash);
}

Status MemMetaTxn::Read(std::string_view hash, std::string_view field, std::string *value) {
  if (value == nullptr || hash.empty()) {
    return Status::InvalidArgument("private chunk index read requires hash and output");
  }
  const auto full_hash = PrivateHash(hash);
  for (auto it = index_writes_.rbegin(); it != index_writes_.rend(); ++it) {
    if (it->hash == full_hash && it->field == field) {
      if (!it->value.has_value()) {
        return Status::NotFound("private chunk index field not found");
      }
      *value = *it->value;
      return Status::OK();
    }
  }
  auto hash_it = store_->private_chunk_index_.find(full_hash);
  if (hash_it == store_->private_chunk_index_.end()) {
    return Status::NotFound("private chunk index hash not found");
  }
  auto field_it = hash_it->second.find(std::string(field));
  if (field_it == hash_it->second.end()) {
    return Status::NotFound("private chunk index field not found");
  }
  *value = field_it->second;
  return Status::OK();
}

Status MemMetaTxn::Scan(std::string_view hash, std::vector<std::pair<std::string, std::string>> *values) {
  if (values == nullptr || hash.empty()) {
    return Status::InvalidArgument("private chunk index scan requires hash and output");
  }
  const auto full_hash = PrivateHash(hash);
  std::map<std::string, std::string> merged;
  auto hash_it = store_->private_chunk_index_.find(full_hash);
  if (hash_it != store_->private_chunk_index_.end()) {
    merged.insert(hash_it->second.begin(), hash_it->second.end());
  }
  for (const auto &write : index_writes_) {
    if (write.hash == full_hash) {
      if (write.value.has_value()) {
        merged.insert_or_assign(write.field, *write.value);
      } else {
        merged.erase(write.field);
      }
    }
  }
  values->assign(merged.begin(), merged.end());
  return Status::OK();
}

Status MemMetaTxn::Put(std::string_view hash, std::string_view field, std::string_view value) {
  if (hash.empty()) {
    return Status::InvalidArgument("private chunk index hash is empty");
  }
  index_writes_.push_back({PrivateHash(hash), std::string(field), std::string(value)});
  return Status::OK();
}

Status MemMetaTxn::Erase(std::string_view hash, std::string_view field) {
  if (hash.empty()) {
    return Status::InvalidArgument("private chunk index hash is empty");
  }
  index_writes_.push_back({PrivateHash(hash), std::string(field), std::nullopt});
  return Status::OK();
}

void MemMetaTxn::CommitLegacyBridgeWrites() {
  for (auto &write : index_writes_) {
    if (write.value.has_value()) {
      store_->private_chunk_index_[write.hash].insert_or_assign(write.field, std::move(*write.value));
    } else {
      auto hash_it = store_->private_chunk_index_.find(write.hash);
      if (hash_it != store_->private_chunk_index_.end()) {
        hash_it->second.erase(write.field);
        if (hash_it->second.empty()) {
          store_->private_chunk_index_.erase(hash_it);
        }
      }
    }
  }
}

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
// Callers compose primitives for POLICY (POSIX error codes, namespace/type
// validation, flag dispatch) and never repeat this bookkeeping. Caller
// authorization is handled before the metadata boundary by Linux VFS/FUSE.
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
  auto status = CheckSetAttrPolicy(inode->attr, fields);
  if (!status.ok()) {
    return status;
  }

  SwordFsAttr st = inode->attr;
  const SwordFsAttr &requested = attr;
  bool size_changed = false;

  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    st.mode = (st.mode & S_IFMT) | (requested.mode & 07777);
  }
  if (HasSetAttrField(fields, SetAttrField::kUid)) {
    st.uid = requested.uid;
  }
  if (HasSetAttrField(fields, SetAttrField::kGid)) {
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
  if (HasSetAttrField(fields, SetAttrField::kKillSuidGid)) {
    st.ClearSetidForKillPriv();
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

  if (size_changed) {
    status = TruncateChunks(ino, st.size);
    if (!status.ok()) {
      return status;
    }
  }
  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    status = SyncPosixAccessAclForMode(inode, st.mode);
  }
  if (status.ok()) {
    status = WriteAttr(ino, st);
  }
  if (!status.ok()) {
    return status;
  }
  if (out) {
    *out = *inode;
  }
  return Status::OK();
}

Status MemMetaTxn::SetInodeFlags(InodeID ino, InodeFlag inode_flags, SwordFsInode *out) {
  if (!HasOnlySupportedInodeFlags(inode_flags)) {
    return Status::InvalidArgument("unsupported inode flags");
  }
  SwordFsInode *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  if (inode->attr.inode_flags != inode_flags) {
    inode->attr.inode_flags = inode_flags;
    inode->Touch(SetAttrField::kCtime);
  }
  if (out != nullptr) {
    *out = *inode;
  }
  return Status::OK();
}

Status MemMetaTxn::SetXAttr(InodeID ino, std::string_view name, std::string_view value, XAttrSetMode mode) {
  SwordFsInode *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  auto status = CheckContentMetadataMutationPolicy(inode->attr, "setxattr");
  if (!status.ok()) {
    return status;
  }
  return inode->SetXAttr(name, value, mode);
}

Status MemMetaTxn::GetXAttr(InodeID ino, std::string_view name, std::string *value) {
  SwordFsInode *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  return inode->GetXAttr(name, value);
}

Status MemMetaTxn::ListXAttrs(InodeID ino, std::vector<std::string> *names) {
  SwordFsInode *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  return inode->ListXAttrs(names);
}

Status MemMetaTxn::RemoveXAttr(InodeID ino, std::string_view name) {
  SwordFsInode *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  auto status = CheckContentMetadataMutationPolicy(inode->attr, "removexattr");
  if (!status.ok()) {
    return status;
  }
  return inode->RemoveXAttr(name);
}

Status MemMetaTxn::Truncate(InodeID ino, uint64_t size) {
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  auto status = CheckContentMetadataMutationPolicy(inode->attr, "truncate");
  if (!status.ok()) {
    return status;
  }
  if (inode->attr.size == static_cast<int64_t>(size)) {
    return Status::OK();
  }

  SwordFsAttr st = inode->attr;
  st.size = size;
  st.mtime = static_cast<int64_t>(::time(nullptr));
  st.mtime_nsec = 0;
  st.ctime = static_cast<int64_t>(::time(nullptr));
  st.ctime_nsec = 0;

  status = TruncateChunks(ino, size);
  if (!status.ok()) {
    return status;
  }
  return WriteAttr(ino, st);
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
  return AddEntry(parent_ino, name, mode, 0, out);
}

Status MemMetaTxn::AddEntry(InodeID parent_ino, std::string_view name, uint32_t mode, uint64_t rdev,
                            SwordFsInode *out) {
  SwordFsInode *parent = FindInode(parent_ino);
  if (!parent) {
    return Status::NotFound("parent directory not found");
  }
  if (!parent->IsDir()) {
    return Status::NotDirectory("parent is not a directory");
  }
  auto status = CheckDirectoryAdditionPolicy(parent->attr);
  if (!status.ok()) {
    return status;
  }
  if (FindEntry(parent_ino, name) != nullptr) {
    return Status::AlreadyExists("entry already exists");
  }

  auto &ctx = folly::fibers::local<swordfs::utils::SwordFsContext>();
  const auto inheritance = ResolveCreateInheritance(ctx.gid, parent->attr, static_cast<uint32_t>(mode));
  SwordFsAttr attr(store_->next_ino_.fetch_add(1, std::memory_order_relaxed), inheritance.mode, ctx.uid,
                   inheritance.gid);
  attr.rdev = rdev;

  auto child = std::make_unique<SwordFsInode>(attr.ino, attr, parent_ino);
  status = ApplyPosixAclCreateInheritance(*parent, static_cast<uint32_t>(ctx.umask), child.get());
  SwordFsInode *child_ptr = child.get();
  if (status.ok()) {
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
  }
  return status;
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
  auto status = CheckDirectoryRemovalPolicy(old_parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckDirectoryAdditionPolicy(new_parent->attr);
  if (!status.ok()) {
    return status;
  }

  SwordFsInode *child = FindEntry(old_parent_ino, old_name);
  if (!child) {
    return Status::NotFound("source entry not found");
  }
  status = CheckContentMetadataMutationPolicy(child->attr, "rename");
  if (!status.ok()) {
    return status;
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
    status = CheckDirectoryRemovalPolicy(new_parent->attr);
    if (!status.ok()) {
      return status;
    }
    status = CheckContentMetadataMutationPolicy(victim->attr, "rename replacement");
    if (!status.ok()) {
      return status;
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
    // name disappears so the background OrphanReclaimer can apply the local open-fd
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

  SwordFsInode *parent = FindInode(parent_ino);
  if (parent == nullptr) {
    return Status::NotFound("parent directory not found");
  }
  auto status = CheckDirectoryRemovalPolicy(parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(child->attr, "unlink");
  if (!status.ok()) {
    return status;
  }

  if (child->IsDir() && !IsDirEmpty(child->ino)) {
    return Status::NotEmpty("directory not empty");
  }

  // Remove the directory entry. Decrement nlink to track the hard-link count.
  // The inode (and its chunks) survive here; durable orphan state tells the
  // background OrphanReclaimer when it may later attempt open-fd-aware preparation.
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
  // until the background OrphanReclaimer confirms no local fd is open and prepares
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
  auto status = CheckDirectoryAdditionPolicy(parent->attr);
  if (!status.ok()) {
    return status;
  }
  if (FindEntry(parent_ino, name) != nullptr) {
    return Status::AlreadyExists("entry already exists");
  }
  SwordFsInode *inode = FindInode(ino);
  if (!inode) {
    return Status::NotFound("inode not found");
  }
  status = CheckContentMetadataMutationPolicy(inode->attr, "link");
  if (!status.ok()) {
    return status;
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
  SwordFsInode *parent_a = FindInode(parent_a_ino);
  SwordFsInode *parent_b = FindInode(parent_b_ino);
  if (parent_a == nullptr || parent_b == nullptr) {
    return Status::NotFound("exchange parent inode not found");
  }
  auto status = CheckDirectoryRemovalPolicy(parent_a->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckDirectoryRemovalPolicy(parent_b->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(inode_a->attr, "rename exchange");
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(inode_b->attr, "rename exchange");
  if (!status.ok()) {
    return status;
  }

  // Two names may be hard links to the same inode. Exchanging those names
  // changes no namespace binding, so it must not rewrite parent metadata or
  // timestamps.
  if (inode_a == inode_b) {
    return Status::OK();
  }

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
  // entries produced by ListEntries point at the right parent. For a
  // same-directory swap both parents are identical, so this is a no-op.
  inode_a->parent_ino = parent_b_ino;
  inode_b->parent_ino = parent_a_ino;

  // Parent nlink tracks directory children through their ".." backlinks.
  // Only a cross-parent cross-type exchange changes the net number of
  // directory children owned by either parent.
  if (parent_a_ino != parent_b_ino && inode_a->IsDir() != inode_b->IsDir()) {
    if (inode_a->IsDir()) {
      parent_a->attr.nlink--;
      parent_b->attr.nlink++;
    } else {
      parent_b->attr.nlink--;
      parent_a->attr.nlink++;
    }
  }

  // Both entries were re-linked: bump ctime on the inodes and
  // mtime/ctime on the parent directories.
  inode_a->Touch(SetAttrField::kCtime);
  inode_b->Touch(SetAttrField::kCtime);
  parent_a->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  parent_b->Touch(SetAttrField::kMtime | SetAttrField::kCtime);

  return Status::OK();
}

Status MemMetaTxn::ReadFileChunkSnapshot(InodeID ino, ChunkIndex index, FileChunkSnapshot *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("file chunk snapshot output is null");
  }
  auto *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  if (!inode->IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }
  FileChunkSnapshot result;
  result.inode = *inode;
  const auto file_it = store_->chunk_refs_.find(ino);
  if (file_it != store_->chunk_refs_.end()) {
    const auto it = file_it->second.find(index);
    if (it != file_it->second.end()) {
      result.chunk_id = it->second;
    }
  }
  ChunkSizeLayout eof_layout;
  auto status = PlanChunkSizeLayout(result.inode.attr.size, store_->chunk_size_, &eof_layout);
  if (!status.ok()) {
    return status;
  }
  if (eof_layout.boundary.has_value()) {
    const auto &boundary = *eof_layout.boundary;
    std::optional<ChunkID> boundary_id;
    if (boundary.index == index) {
      boundary_id = result.chunk_id;
    } else {
      status = ProbeAttachment(ino, boundary.index, &boundary_id);
      if (!status.ok()) {
        return status;
      }
    }
    result.eof_boundary = ChunkBoundarySnapshot{
        .index = boundary.index, .chunk_id = boundary_id, .visible_prefix = boundary.visible_prefix};
  }
  *out = std::move(result);
  return Status::OK();
}

Status MemMetaTxn::ReadFileMappingSnapshot(InodeID ino, FileMappingSnapshot *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("file mapping snapshot output is null");
  }
  auto *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  if (!inode->IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }
  FileMappingSnapshot result;
  result.inode = *inode;
  const auto file_it = store_->chunk_refs_.find(ino);
  if (file_it != store_->chunk_refs_.end()) {
    result.mappings.reserve(file_it->second.size());
    for (const auto &[index, chunk_id] : file_it->second) {
      result.mappings.push_back({.index = index, .chunk_id = chunk_id});
    }
    std::sort(result.mappings.begin(), result.mappings.end(),
              [](const ChunkMapping &lhs, const ChunkMapping &rhs) { return lhs.index < rhs.index; });
  }
  *out = std::move(result);
  return Status::OK();
}

Status MemMetaTxn::ProbeAttachment(InodeID ino, ChunkIndex index, std::optional<ChunkID> *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("attachment output is null");
  }
  if (FindInode(ino) == nullptr) {
    return Status::NotFound("inode not found");
  }
  out->reset();
  const auto file_it = store_->chunk_refs_.find(ino);
  if (file_it != store_->chunk_refs_.end()) {
    const auto it = file_it->second.find(index);
    if (it != file_it->second.end()) {
      *out = it->second;
    }
  }
  return Status::OK();
}

Status MemMetaTxn::AttachPrepared(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                                  const FileSizePrecondition &expected) {
  auto status = ValidateFileChunkWrite(index, chunk_id, end, store_->chunk_size_);
  if (!status.ok()) {
    return status;
  }
  status = ValidateFileSizePrecondition(expected, store_->chunk_size_);
  if (!status.ok()) {
    return status;
  }
  auto *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  if (!inode->IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }
  status = CheckContentMetadataMutationPolicy(inode->attr, "attach chunk");
  if (!status.ok()) {
    return status;
  }
  if (inode->attr.size != expected.eof) {
    return Status::AlreadyExists("file size changed before chunk attachment");
  }
  if (expected.boundary.has_value()) {
    std::optional<ChunkID> observed;
    status = ProbeAttachment(ino, expected.boundary->index, &observed);
    if (!status.ok()) {
      return status;
    }
    if (observed != expected.boundary->chunk_id) {
      return Status::AlreadyExists("FileMetadata EOF boundary changed before attachment");
    }
  }
  const auto file_it = store_->chunk_refs_.find(ino);
  if (file_it != store_->chunk_refs_.end() && file_it->second.contains(index)) {
    return Status::AlreadyExists("chunk is already attached");
  }
  store_->chunk_refs_[ino].emplace(index, chunk_id);
  inode->attr.size = std::max(inode->attr.size, end);
  inode->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return Status::OK();
}

Status MemMetaTxn::FinalizeAttachedWrite(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                                         const FileSizePrecondition &expected) {
  auto status = ValidateFileChunkWrite(index, chunk_id, end, store_->chunk_size_);
  if (!status.ok()) {
    return status;
  }
  status = ValidateFileSizePrecondition(expected, store_->chunk_size_);
  if (!status.ok()) {
    return status;
  }
  auto *inode = FindInode(ino);
  if (inode == nullptr) {
    return Status::NotFound("inode not found");
  }
  if (!inode->IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }
  status = CheckContentMetadataMutationPolicy(inode->attr, "finalize chunk write");
  if (!status.ok()) {
    return status;
  }
  const auto file_it = store_->chunk_refs_.find(ino);
  if (inode->attr.size != expected.eof || file_it == store_->chunk_refs_.end() || !file_it->second.contains(index) ||
      file_it->second.at(index) != chunk_id) {
    return Status::AlreadyExists("FileMetadata attachment or size changed before finalization");
  }
  if (expected.boundary.has_value()) {
    std::optional<ChunkID> observed;
    status = ProbeAttachment(ino, expected.boundary->index, &observed);
    if (!status.ok()) {
      return status;
    }
    if (observed != expected.boundary->chunk_id) {
      return Status::AlreadyExists("FileMetadata EOF boundary changed before finalization");
    }
  }
  inode->attr.size = std::max(inode->attr.size, end);
  inode->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return Status::OK();
}

Status MemMetaTxn::CommitShrink(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                SetAttrField fields, ChunkSizeCommitResult *out) {
  return CommitSizeChange(ino, plan, requested, fields, /*is_shrink=*/true, out);
}

Status MemMetaTxn::CommitGrow(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested, SetAttrField fields,
                              ChunkSizeCommitResult *out) {
  return CommitSizeChange(ino, plan, requested, fields, /*is_shrink=*/false, out);
}

Status MemMetaTxn::CommitSizeChange(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                    SetAttrField fields, bool is_shrink, ChunkSizeCommitResult *out) {
  if (out == nullptr || !HasSetAttrField(fields, SetAttrField::kSize) || requested.size != plan.target_eof) {
    return Status::InvalidArgument("typed size commit requires matching SetAttr size and result");
  }
  FileMappingSnapshot snapshot;
  auto status = ReadFileMappingSnapshot(ino, &snapshot);
  if (!status.ok()) {
    return status;
  }
  ChunkSizePlan current;
  status = ValidateSizeCommitPlan(plan, snapshot.inode.attr.size, store_->chunk_size_, snapshot.mappings, is_shrink,
                                  &current);
  if (!status.ok()) {
    return status;
  }

  // All potentially failing ACL and policy checks run against a clone before
  // the authoritative inode or any attachment is mutated.
  SwordFsInode updated;
  status = PrepareSetAttrMutation(snapshot.inode, requested, fields, &updated);
  if (!status.ok()) {
    return status;
  }

  ChunkSizeCommitResult result{.inode = updated, .boundary = current.boundary};
  ChunkSizeLayout old_layout;
  status = PlanChunkSizeLayout(snapshot.inode.attr.size, store_->chunk_size_, &old_layout);
  if (!status.ok()) {
    return status;
  }
  ChunkSizeLayout new_layout;
  status = PlanChunkSizeLayout(plan.target_eof, store_->chunk_size_, &new_layout);
  if (!status.ok()) {
    return status;
  }
  for (const auto &mapping : snapshot.mappings) {
    if ((is_shrink && new_layout.ShouldDetach(mapping.index)) ||
        (!is_shrink && old_layout.ShouldDetach(mapping.index))) {
      // A future grow must never reactivate a mapping left past EOF by an
      // earlier uncertain shrink. Treat it as newly detached maintenance.
      result.detached.push_back(mapping);
    }
  }
  if (!result.detached.empty()) {
    if (store_->chunk_metadata_bridge_ == nullptr) {
      return Status::Internal("chunk metadata bridge is not bound");
    }
    // A detached ChunkID is no longer reachable through FileMetadata after
    // this transaction. Record maintenance work before mutating the map;
    // the cleanup worker still revalidates attachment before deletion.
    for (const auto &mapping : result.detached) {
      PendingDelete pending;
      status = store_->chunk_metadata_bridge_->FreezeDetachedDelete(ino, mapping, &pending);
      if (!status.ok()) {
        return status;
      }
      store_->pending_deletes_.insert_or_assign(pending.id, std::move(pending));
    }
  }
  auto refs_it = store_->chunk_refs_.find(ino);
  if (refs_it != store_->chunk_refs_.end()) {
    for (const auto &detached : result.detached) {
      refs_it->second.erase(detached.index);
    }
    if (refs_it->second.empty()) {
      store_->chunk_refs_.erase(refs_it);
    }
  }
  *FindInode(ino) = std::move(updated);
  *out = std::move(result);
  return Status::OK();
}

Status MemMetaTxn::CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                               const SwordFsChunk &replacement, const ChunkPublishIntent &intent) {
  if (replacement.revision == kInvalidChunkRevision ||
      (expected.has_value() && expected->revision == kInvalidChunkRevision)) {
    return Status::InvalidArgument("chunk revision is invalid");
  }
  if (expected.has_value() && replacement.revision <= expected->revision) {
    return Status::InvalidArgument("replacement revision must increase");
  }
  if (expected.has_value() && expected->index != replacement.index) {
    return Status::InvalidArgument("replacement must preserve chunk index");
  }
  if (store_->chunk_metadata_bridge_ == nullptr) {
    return Status::Internal("chunk metadata bridge is not bound");
  }
  const auto &bridge = *store_->chunk_metadata_bridge_;
  auto queue_pending_delete = [&](const SwordFsChunk &head) {
    PendingDelete pending;
    auto status = bridge.FreezeRejectedPublication(ino, head, intent, store_->chunk_size_, &pending);
    if (status.ok()) {
      store_->pending_deletes_.insert_or_assign(pending.id, std::move(pending));
    }
  };

  auto *inode = FindInode(ino);
  if (inode == nullptr) {
    queue_pending_delete(replacement);
    return Status::NotFound("inode not found: " + std::to_string(ino));
  }
  if (!inode->IsRegular()) {
    return Status::InvalidArgument("not a regular file");
  }
  uint64_t start_offset = 0;
  auto status = CalculateChunkStartOffset(replacement.index, store_->chunk_size_, &start_offset);
  if (!status.ok() || replacement.size > kMaxSupportedFileSize - start_offset) {
    return Status::InvalidArgument("replacement chunk extent is invalid");
  }

  auto &chunk_map = store_->chunks_[ino];
  auto it = chunk_map.find(replacement.index);
  bool replacement_already_published = false;
  std::optional<PendingDelete> expected_cleanup;
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
      PendingDelete pending;
      auto status = bridge.FreezePendingDelete(*this, ino, *expected, store_->chunk_size_, &pending);
      if (!status.ok()) {
        return status;
      }
      expected_cleanup = std::move(pending);
    }
  } else if (expected.has_value()) {
    queue_pending_delete(replacement);
    return Status::NotFound("chunk not found at index " + std::to_string(replacement.index));
  }

  status = bridge.Publish(*this, ino, expected, replacement, intent);
  if (!status.ok()) {
    queue_pending_delete(replacement);
    return status;
  }
  if (expected_cleanup.has_value()) {
    store_->pending_deletes_.insert_or_assign(expected_cleanup->id, std::move(*expected_cleanup));
  }

  if (!replacement_already_published) {
    chunk_map[replacement.index] = replacement;
  }

  const uint64_t chunk_end = start_offset + replacement.size;
  if (chunk_end > inode->attr.size) {
    inode->attr.size = chunk_end;
  }
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

Status MemMetaTxn::LoadChunkView(InodeID ino, ChunkIndex idx, ChunkView *out) {
  if (out == nullptr) {
    return Status::InvalidArgument("chunk view output is null");
  }
  ChunkView view;
  auto status = FindChunk(ino, idx, &view.head);
  if (!status.ok()) {
    return status;
  }
  if (!view.head.IsValidForChunkSize(store_->chunk_size_)) {
    return Status::Malformed("persisted chunk descriptor is invalid");
  }
  // A chunk can only enter the store through CommitChunk(), which fails
  // closed until the mount-time bridge has been bound.
  const auto &bridge = *store_->chunk_metadata_bridge_;
  status = bridge.LoadPublished(*this, ino, view.head, &view.private_snapshot);
  if (!status.ok()) {
    return status;
  }
  *out = std::move(view);
  return Status::OK();
}

Status MemMetaTxn::TruncateChunks(InodeID ino, uint64_t new_size) {
  auto ino_it = store_->chunks_.find(ino);
  if (ino_it == store_->chunks_.end()) {
    return Status::OK();
  }
  auto &cmap = ino_it->second;
  // Non-empty chunk maps imply a successful CommitChunk(), and therefore
  // an already-bound mount-time bridge.
  const auto &bridge = *store_->chunk_metadata_bridge_;
  ChunkSizeLayout layout;
  auto status = PlanChunkSizeLayout(new_size, store_->chunk_size_, &layout);
  if (!status.ok()) {
    return status;
  }
  std::vector<ChunkIndexChange> changes;
  std::vector<PendingDelete> detached;
  for (const auto &[index, head] : cmap) {
    (void)index;
    if (layout.ShouldDetach(head.index)) {
      PendingDelete pending;
      status = bridge.FreezePendingDelete(*this, ino, head, store_->chunk_size_, &pending);
      if (!status.ok()) {
        return status;
      }
      detached.push_back(std::move(pending));
      changes.push_back({head, std::nullopt});
    } else if (layout.boundary.has_value() && head.index == layout.boundary->index &&
               head.size > layout.boundary->visible_prefix) {
      auto clamped = head;
      clamped.size = layout.boundary->visible_prefix;
      changes.push_back({head, clamped});
    }
  }
  for (auto &pending : detached) {
    store_->pending_deletes_.insert_or_assign(pending.id, std::move(pending));
  }
  for (const auto &change : changes) {
    if (change.current.has_value()) {
      cmap[change.previous.index] = *change.current;
    } else {
      cmap.erase(change.previous.index);
    }
  }
  return Status::OK();
}

Status MemMetaTxn::PrepareReclaim(InodeID ino, std::optional<ReclaimWork> *work) {
  if (work == nullptr) {
    return Status::InvalidArgument("reclaim work output is null");
  }
  work->reset();

  // Cleanup work is maintenance state, not reclaim authority. Consult the
  // live inode first so a stale handoff can never override a revived inode.
  auto pending_it = store_->pending_reclaims_.find(ino);
  SwordFsInode *inode = FindInode(ino);
  if (inode == nullptr) {
    // The logical point of no return was already crossed. Preserve any
    // remaining maintenance work for GC, but its presence is not required.
    store_->orphans_.erase(ino);
    if (pending_it != store_->pending_reclaims_.end()) {
      *work = pending_it->second;
    }
    return Status::OK();
  }
  if (inode->IsDir() || inode->attr.nlink != 0) {
    // A concurrent Link revived the inode before this transaction, or the
    // node is a directory. Drop stale maintenance state together with the
    // orphan marker; neither can authorize cleanup of a live inode.
    store_->orphans_.erase(ino);
    store_->pending_reclaims_.erase(ino);
    return Status::OK();
  }

  std::vector<SwordFsChunk> heads;
  auto chunks_it = store_->chunks_.find(ino);
  if (chunks_it != store_->chunks_.end()) {
    heads.reserve(chunks_it->second.size());
    for (const auto &[index, head] : chunks_it->second) {
      (void)index;
      heads.push_back(head);
    }
    std::sort(heads.begin(), heads.end(),
              [](const SwordFsChunk &a, const SwordFsChunk &b) { return a.index < b.index; });
  }
  if (store_->chunk_metadata_bridge_ == nullptr) {
    return Status::Internal("chunk metadata bridge is not bound");
  }
  const auto &bridge = *store_->chunk_metadata_bridge_;
  std::vector<ChunkMapping> attached;
  if (const auto refs_it = store_->chunk_refs_.find(ino); refs_it != store_->chunk_refs_.end()) {
    attached.reserve(refs_it->second.size());
    for (const auto &[index, chunk_id] : refs_it->second) {
      attached.push_back({.index = index, .chunk_id = chunk_id});
    }
    std::sort(attached.begin(), attached.end(),
              [](const ChunkMapping &a, const ChunkMapping &b) { return a.index < b.index; });
  }
  ReclaimWork frozen;
  // The typed attachment map, when present, is the only live publication
  // authority. Never derive typed cleanup from the unrelated legacy table.
  auto status = !attached.empty() ? bridge.FreezeDetachedReclaim(ino, attached, &frozen)
                                  : bridge.FreezeReclaim(*this, ino, heads, store_->chunk_size_, &frozen);
  if (!status.ok()) {
    return status;
  }
  if (attached.empty()) {
    status = bridge.PrepareReclaim(*this, ino, heads);
    if (!status.ok()) {
      return status;
    }
  }

  // Memory performs the non-revivability and descriptor detach atomically
  // under the store transaction. The cleanup handoff is published only after
  // that correctness transition; losing it would leak data, not revive it.
  DeleteInode(ino);
  store_->chunk_refs_.erase(ino);
  store_->orphans_.erase(ino);
  store_->pending_reclaims_[ino] = frozen;

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
