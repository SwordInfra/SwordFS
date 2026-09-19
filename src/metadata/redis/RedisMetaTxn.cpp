// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaTxn.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <sys/stat.h>

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "chunk/ChunkObjectKey.hpp"
#include "metadata/Utils.hpp"
#include "metadata/redis/RedisKvTxn.hpp"

namespace swordfs::metadata {

RedisMetaTxn::RedisMetaTxn(RedisKvTxn &txn, const redis::RedisKey &key, uint64_t chunk_size)
    : txn_(txn), key_(key), chunk_size_(chunk_size) {
}

utils::Status RedisMetaTxn::LookupInode(InodeID ino, SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }

  std::string value;
  auto status = txn_.Get(key_.Inode(ino), &value);
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(value);
}

utils::Status RedisMetaTxn::LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }
  SwordFsInode parent;
  auto status = LookupInode(parent_ino, &parent);
  if (!status.ok()) {
    return status;
  }
  return LookupEntry(parent, name, out);
}

utils::Status RedisMetaTxn::LookupEntry(const SwordFsInode &parent, std::string_view name, SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }
  if (!parent.IsDir()) {
    return utils::Status::NotDirectory("parent is not a directory");
  }

  std::string value;
  auto status = txn_.HGet(key_.Directory(parent.ino), name, &value);
  if (!status.ok()) {
    return status;
  }
  SwordFsEntry entry;
  status = entry.ParseFrom(value);
  if (!status.ok()) {
    return status;
  }
  status = LookupInode(entry.ino, out);
  if (status.IsNotFound()) {
    return utils::Status::Malformed("directory entry references missing inode");
  }
  return status;
}

utils::Status RedisMetaTxn::IsDescendantOf(InodeID ancestor_ino, InodeID child_ino, bool *result) {
  if (result == nullptr) {
    return utils::Status::InvalidArgument("descendant check output is null");
  }
  *result = false;
  InodeID current_ino = child_ino;
  folly::F14FastSet<InodeID> visited;
  while (current_ino != 0) {
    if (!visited.insert(current_ino).second) {
      return utils::Status::Malformed("directory parent cycle detected");
    }
    if (current_ino == ancestor_ino) {
      *result = true;
      return utils::Status::OK();
    }
    SwordFsInode inode;
    auto status = LookupInode(current_ino, &inode);
    if (!status.ok()) {
      return status;
    }
    if (inode.parent_ino == current_ino) {
      break;
    }
    current_ino = inode.parent_ino;
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::SetInode(const SwordFsInode &inode) {
  if (inode.ino == 0 || inode.attr.ino != inode.ino) {
    return utils::Status::InvalidArgument("invalid inode record");
  }
  std::string value;
  auto status = inode.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  return txn_.Set(key_.Inode(inode.ino), value);
}

utils::Status RedisMetaTxn::DeleteInode(InodeID ino) {
  auto status = txn_.Del(key_.Inode(ino));
  if (!status.ok()) {
    return status;
  }
  return AdjustInodeCount(-1);
}

utils::Status RedisMetaTxn::AdjustNlink(SwordFsInode *inode, int delta, uint64_t *nlink) {
  if (inode == nullptr) {
    return utils::Status::InvalidArgument("inode is null");
  }
  if (delta < 0) {
    const uint64_t amount = static_cast<uint64_t>(-static_cast<int64_t>(delta));
    if (inode->attr.nlink < amount) {
      return utils::Status::Malformed("inode link count underflow");
    }
    inode->attr.nlink -= amount;
  } else {
    inode->attr.nlink += static_cast<uint64_t>(delta);
  }
  if (nlink != nullptr) {
    *nlink = inode->attr.nlink;
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out,
                                    std::vector<SwordFsChunk> *detached_chunks) {
  if (detached_chunks != nullptr) {
    detached_chunks->clear();
  }
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }

  SwordFsAttr attr = inode.attr;
  const uint64_t old_size = attr.size;
  const bool size_changed = HasSetAttrField(fields, SetAttrField::kSize) && old_size != requested.size;
  const bool owner_changed = (HasSetAttrField(fields, SetAttrField::kUid) && attr.uid != requested.uid) ||
                             (HasSetAttrField(fields, SetAttrField::kGid) && attr.gid != requested.gid);

  if (HasSetAttrField(fields, SetAttrField::kSize)) {
    // Redis MULTI/EXEC can partially apply a chunk descriptor update while
    // leaving inode.size unchanged. Even a size-preserving setattr must
    // reconcile chunk metadata so a later retry does not observe a descriptor
    // beyond the requested EOF.
    status = TruncateChunks(ino, old_size, requested.size, detached_chunks);
    if (!status.ok()) {
      return status;
    }
  }
  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    attr.mode = (attr.mode & S_IFMT) | (requested.mode & 07777);
  }
  if (HasSetAttrField(fields, SetAttrField::kUid)) {
    attr.uid = requested.uid;
  }
  if (HasSetAttrField(fields, SetAttrField::kGid)) {
    attr.gid = requested.gid;
  }
  if (HasSetAttrField(fields, SetAttrField::kSize)) {
    attr.size = requested.size;
    if (!HasSetAttrField(fields, SetAttrField::kMtime) && !HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
      inode.attr = attr;
      inode.Touch(SetAttrField::kMtime);
      attr = inode.attr;
    }
  }
  if (HasSetAttrField(fields, SetAttrField::kAtime)) {
    attr.atime = requested.atime;
    attr.atime_nsec = requested.atime_nsec;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtime)) {
    attr.mtime = requested.mtime;
    attr.mtime_nsec = requested.mtime_nsec;
  }
  if (HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
    inode.attr = attr;
    inode.Touch(SetAttrField::kAtime);
    attr = inode.attr;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
    inode.attr = attr;
    inode.Touch(SetAttrField::kMtime);
    attr = inode.attr;
  }
  if (HasSetAttrField(fields, SetAttrField::kCtime)) {
    attr.ctime = requested.ctime;
    attr.ctime_nsec = requested.ctime_nsec;
  }
  if (size_changed || owner_changed) {
    attr.KillSUID();
  }
  if (!HasSetAttrField(fields, SetAttrField::kCtime)) {
    inode.attr = attr;
    inode.Touch(SetAttrField::kCtime);
    attr = inode.attr;
  }

  inode.attr = attr;
  status = SetInode(inode);
  if (!status.ok()) {
    return status;
  }
  if (out != nullptr) {
    *out = inode;
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::Truncate(InodeID ino, uint64_t size, std::vector<SwordFsChunk> *detached_chunks) {
  if (detached_chunks != nullptr) {
    detached_chunks->clear();
  }
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  status = TruncateChunks(ino, inode.attr.size, size, detached_chunks);
  if (!status.ok()) {
    return status;
  }
  if (inode.attr.size == size) {
    return utils::Status::OK();
  }
  inode.attr.size = size;
  inode.attr.KillSUID();
  inode.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return SetInode(inode);
}

utils::Status RedisMetaTxn::TouchInode(InodeID ino, SetAttrField fields) {
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  inode.Touch(fields);
  return SetInode(inode);
}

utils::Status RedisMetaTxn::PrepareReclaim(InodeID ino, std::optional<ReclaimWork> &work) {
  work.reset();

  // Idempotent replay: once the point of no return has been crossed the
  // frozen record — not the (already removed) live inode — is the authority,
  // so crash recovery and retries get the same work back unchanged.
  std::string existing;
  auto status = txn_.HGet(key_.Reclaims(), std::to_string(ino), &existing);
  if (status.ok()) {
    ReclaimWork pending;
    status = pending.ParseFrom(existing);
    if (!status.ok()) {
      return status;
    }
    if (pending.ino != ino) {
      return utils::Status::Malformed("pending reclaim record inode mismatch");
    }
    work = std::move(pending);
    return utils::Status::OK();
  }
  if (!status.IsNotFound()) {
    return status;
  }

  // All remaining reads happen before the first write. The live inode and the
  // complete chunk hash are WATCHed on the same Redis connection, so a
  // concurrent metadata mutation aborts EXEC and RedisMetaClient retries the
  // whole attempt from a fresh snapshot.
  SwordFsInode inode;
  status = LookupInode(ino, &inode);
  const bool has_inode = status.ok();
  if (!has_inode && !status.IsNotFound()) {
    return status;
  }

  if (!has_inode || inode.IsDir() || inode.attr.nlink != 0) {
    // The inode was already reclaimed, is a directory (never reclaimed
    // through an orphan candidate), or a concurrent Link revived it: it must
    // not be frozen. Dropping the orphan marker in this same transaction is
    // what makes that safe — the inode key is watched, so a concurrent unlink
    // that publishes the marker either lands before this EXEC (and is then
    // observed, so we freeze instead) or aborts it and is retried.
    status = ClearOrphanMarker(ino);
    if (!status.ok()) {
      return status;
    }
    return utils::Status::OK();
  }

  std::vector<std::pair<std::string, SwordFsChunk>> scanned;
  status = ScanChunks(ino, scanned);
  if (!status.ok()) {
    return status;
  }

  ReclaimWork pending;
  pending.ino = ino;
  pending.chunks.reserve(scanned.size());
  for (const auto &[field, descriptor] : scanned) {
    (void)field;
    // Freeze the object identity the authoritative descriptor derives right
    // now; deletions replay these keys verbatim from the frozen record.
    pending.chunks.push_back(
        ReclaimChunk{descriptor, chunk::FormatChunkObjectKey(ino, descriptor.index, descriptor.revision)});
  }
  std::sort(pending.chunks.begin(), pending.chunks.end(),
            [](const ReclaimChunk &a, const ReclaimChunk &b) { return a.descriptor.index < b.descriptor.index; });

  std::string serialized;
  status = pending.SerializeTo(&serialized);
  if (!status.ok()) {
    return status;
  }

  // Retain the frozen work durably, then drop the live chunk map, the live
  // inode and the orphan marker. This is the point of no return: from here on
  // no Link can revive the inode and every remaining step is idempotent.
  status = txn_.HSet(key_.Reclaims(), std::to_string(ino), serialized);
  if (!status.ok()) {
    return status;
  }
  status = ClearOrphanMarker(ino);
  if (!status.ok()) {
    return status;
  }
  status = DeleteChunks(ino);
  if (!status.ok()) {
    return status;
  }
  status = DeleteInode(ino);
  if (!status.ok()) {
    return status;
  }

  work = std::move(pending);
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::CompleteReclaim(InodeID ino) {
  return txn_.HDel(key_.Reclaims(), std::to_string(ino));
}

utils::Status RedisMetaTxn::CompletePendingDelete(std::string_view object_key) {
  return txn_.HDel(key_.PendingDeletes(), object_key);
}

utils::Status RedisMetaTxn::ClearOrphanMarker(InodeID ino) {
  return txn_.HDel(key_.Orphans(), std::to_string(ino));
}

utils::Status RedisMetaTxn::AddEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                                     SwordFsInode *parent) {
  if (parent == nullptr) {
    return utils::Status::InvalidArgument("parent inode is null");
  }
  if (parent->ino != parent_ino || child.parent_ino != parent_ino) {
    return utils::Status::InvalidArgument("entry parent mismatch");
  }

  std::string existing_value;
  auto status = txn_.HGet(key_.Directory(parent_ino), name, &existing_value);
  if (status.ok()) {
    SwordFsEntry existing_entry;
    status = existing_entry.ParseFrom(existing_value);
    if (!status.ok()) {
      return status;
    }
    SwordFsInode existing_inode;
    status = LookupInode(existing_entry.ino, &existing_inode);
    if (status.IsNotFound()) {
      return utils::Status::Malformed("directory entry references missing inode");
    }
    if (!status.ok()) {
      return status;
    }
    return utils::Status::AlreadyExists("entry already exists");
  }
  if (!status.IsNotFound()) {
    return status;
  }

  status = SetInode(child);
  if (!status.ok()) {
    return status;
  }
  status = LinkEntry(parent_ino, name, child, parent);
  if (!status.ok()) {
    return status;
  }
  status = SetInode(*parent);
  if (!status.ok()) {
    return status;
  }
  return AdjustInodeCount(1);
}

utils::Status RedisMetaTxn::UnlinkFile(InodeID parent_ino, std::string_view name, SwordFsInode *parent,
                                       SwordFsInode *child) {
  if (parent == nullptr || child == nullptr) {
    return utils::Status::InvalidArgument("unlink inode is null");
  }
  if (parent->ino != parent_ino) {
    return utils::Status::InvalidArgument("entry parent mismatch");
  }
  if (child->IsDir()) {
    return utils::Status::InvalidArgument("cannot unlink directory");
  }

  auto status = AdjustNlink(child, -1);
  if (!status.ok()) {
    return status;
  }
  child->Touch(SetAttrField::kCtime);
  status = DetachEntry(parent_ino, name, *child, parent);
  if (!status.ok()) {
    return status;
  }
  status = SetInode(*parent);
  if (!status.ok()) {
    return status;
  }
  status = SetInode(*child);
  if (!status.ok()) {
    return status;
  }
  if (child->attr.nlink == 0) {
    // Last name gone: publish the inode as an orphan candidate in the same
    // transaction as the nlink decrement, so the fact that it is unreachable
    // survives a crash before the caller gets to reclaim it.
    status = txn_.HSet(key_.Orphans(), std::to_string(child->ino), "1");
    if (!status.ok()) {
      return status;
    }
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::RemoveDirectory(InodeID parent_ino, std::string_view name, SwordFsInode *parent,
                                            const SwordFsInode &child) {
  if (parent == nullptr) {
    return utils::Status::InvalidArgument("parent inode is null");
  }
  if (parent->ino != parent_ino || child.parent_ino != parent_ino) {
    return utils::Status::InvalidArgument("entry parent mismatch");
  }
  if (!child.IsDir()) {
    return utils::Status::NotDirectory("not a directory");
  }

  uint64_t length = 0;
  auto status = txn_.HLen(key_.Directory(child.ino), &length);
  if (!status.ok()) {
    return status;
  }
  if (length != 0) {
    return utils::Status::NotEmpty("directory not empty");
  }
  status = DetachEntry(parent_ino, name, child, parent);
  if (!status.ok()) {
    return status;
  }
  status = DeleteInode(child.ino);
  if (!status.ok()) {
    return status;
  }
  return SetInode(*parent);
}

utils::Status RedisMetaTxn::MoveEntry(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                                      std::string_view new_name, SwordFsInode *old_parent, SwordFsInode *new_parent,
                                      SwordFsInode *source, SwordFsInode *target, bool overwrite) {
  if (old_parent == nullptr || new_parent == nullptr || source == nullptr) {
    return utils::Status::InvalidArgument("rename inode is null");
  }
  if (old_parent->ino != old_parent_ino || new_parent->ino != new_parent_ino) {
    return utils::Status::InvalidArgument("rename parent mismatch");
  }
  if ((old_parent_ino == new_parent_ino) != (old_parent == new_parent)) {
    return utils::Status::InvalidArgument("rename parent state mismatch");
  }
  if (!old_parent->IsDir() || !new_parent->IsDir()) {
    return utils::Status::NotDirectory("rename parent is not a directory");
  }

  if (source->IsDir()) {
    bool cycle = false;
    auto status = IsDescendantOf(source->ino, new_parent_ino, &cycle);
    if (!status.ok()) {
      return status;
    }
    if (cycle) {
      return utils::Status::InvalidArgument("cannot move directory into its descendant");
    }
  }

  if (target != nullptr) {
    if (!overwrite) {
      return utils::Status::AlreadyExists("target entry exists");
    }
    if (target->ino == source->ino) {
      return utils::Status::OK();
    }
    if (source->IsDir() != target->IsDir()) {
      return source->IsDir() ? utils::Status::NotDirectory("target is not a directory")
                             : utils::Status::IsDirectory("target is a directory");
    }

    if (target->IsDir()) {
      auto status = RemoveDirectory(new_parent_ino, new_name, new_parent, *target);
      if (!status.ok()) {
        return status;
      }
    } else {
      auto status = UnlinkFile(new_parent_ino, new_name, new_parent, target);
      if (!status.ok()) {
        return status;
      }
    }
  }

  auto status = DetachEntry(old_parent_ino, old_name, *source, old_parent);
  if (!status.ok()) {
    return status;
  }
  status = LinkEntry(new_parent_ino, new_name, *source, new_parent);
  if (!status.ok()) {
    return status;
  }
  source->parent_ino = new_parent_ino;
  source->Touch(SetAttrField::kCtime);
  status = SetInode(*old_parent);
  if (!status.ok()) {
    return status;
  }
  if (new_parent != old_parent) {
    status = SetInode(*new_parent);
    if (!status.ok()) {
      return status;
    }
  }
  return SetInode(*source);
}

utils::Status RedisMetaTxn::ExchangeEntries(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                                            std::string_view new_name, SwordFsInode *old_parent,
                                            SwordFsInode *new_parent, SwordFsInode *source, SwordFsInode *target) {
  if (old_parent == nullptr || new_parent == nullptr || source == nullptr || target == nullptr) {
    return utils::Status::InvalidArgument("exchange inode is null");
  }
  if (old_parent->ino != old_parent_ino || new_parent->ino != new_parent_ino) {
    return utils::Status::InvalidArgument("exchange parent mismatch");
  }
  if ((old_parent_ino == new_parent_ino) != (old_parent == new_parent)) {
    return utils::Status::InvalidArgument("exchange parent state mismatch");
  }
  if (!old_parent->IsDir() || !new_parent->IsDir()) {
    return utils::Status::NotDirectory("exchange parent is not a directory");
  }
  if (source->ino == target->ino) {
    return utils::Status::OK();
  }
  if (source->IsDir() != target->IsDir()) {
    return utils::Status::InvalidArgument("cannot exchange directory with non-directory");
  }

  if (source->IsDir()) {
    bool cycle = false;
    auto status = IsDescendantOf(source->ino, new_parent_ino, &cycle);
    if (!status.ok()) {
      return status;
    }
    if (cycle) {
      return utils::Status::InvalidArgument("cannot exchange directory into its descendant");
    }
    status = IsDescendantOf(target->ino, old_parent_ino, &cycle);
    if (!status.ok()) {
      return status;
    }
    if (cycle) {
      return utils::Status::InvalidArgument("cannot exchange directory into its descendant");
    }
  }

  auto status = ReplaceEntry(old_parent_ino, old_name, *target, old_parent);
  if (!status.ok()) {
    return status;
  }
  status = ReplaceEntry(new_parent_ino, new_name, *source, new_parent);
  if (!status.ok()) {
    return status;
  }
  source->parent_ino = new_parent_ino;
  target->parent_ino = old_parent_ino;
  source->Touch(SetAttrField::kCtime);
  target->Touch(SetAttrField::kCtime);
  status = SetInode(*old_parent);
  if (!status.ok()) {
    return status;
  }
  if (new_parent != old_parent) {
    status = SetInode(*new_parent);
    if (!status.ok()) {
      return status;
    }
  }
  status = SetInode(*source);
  if (!status.ok()) {
    return status;
  }
  return SetInode(*target);
}

utils::Status RedisMetaTxn::LinkExistingEntry(InodeID parent_ino, std::string_view name, SwordFsInode *parent,
                                              SwordFsInode *inode) {
  if (parent == nullptr || inode == nullptr) {
    return utils::Status::InvalidArgument("link inode is null");
  }
  if (parent->ino != parent_ino) {
    return utils::Status::InvalidArgument("entry parent mismatch");
  }
  if (!parent->IsDir()) {
    return utils::Status::NotDirectory("parent is not a directory");
  }
  if (inode->IsDir()) {
    return utils::Status::NotPermitted("cannot hard-link directory");
  }

  SwordFsInode existing;
  auto status = LookupEntry(*parent, name, &existing);
  if (status.ok()) {
    return utils::Status::AlreadyExists("entry already exists");
  }
  if (!status.IsNotFound()) {
    return status;
  }

  status = LinkEntry(parent_ino, name, *inode, parent);
  if (!status.ok()) {
    return status;
  }
  // A revived orphan candidate (nlink 0 -> 1) is no longer an orphan. Drop
  // the marker in the same transaction that re-links it so reconciliation
  // can never reclaim an inode that has a name again.
  const bool was_orphan = (inode->attr.nlink == 0);
  status = AdjustNlink(inode, 1);
  if (!status.ok()) {
    return status;
  }
  if (was_orphan) {
    status = ClearOrphanMarker(inode->ino);
    if (!status.ok()) {
      return status;
    }
  }
  inode->Touch(SetAttrField::kCtime);
  status = SetInode(*parent);
  if (!status.ok()) {
    return status;
  }
  return SetInode(*inode);
}

utils::Status RedisMetaTxn::LinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                                      SwordFsInode *parent) {
  if (parent == nullptr) {
    return utils::Status::InvalidArgument("parent inode is null");
  }
  if (!parent->IsDir()) {
    return utils::Status::NotDirectory("parent is not a directory");
  }

  if (child.IsDir()) {
    parent->attr.nlink++;
  }
  parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);

  SwordFsEntry entry{std::string(name), ModeToDt(child.attr.mode), child.ino};
  std::string value;
  auto status = entry.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  return txn_.HSet(key_.Directory(parent_ino), name, value);
}

utils::Status RedisMetaTxn::DetachEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &target,
                                        SwordFsInode *parent) {
  if (parent == nullptr) {
    return utils::Status::InvalidArgument("parent inode is null");
  }
  if (!parent->IsDir()) {
    return utils::Status::NotDirectory("parent is not a directory");
  }

  if (target.IsDir()) {
    if (parent->attr.nlink <= 2) {
      return utils::Status::Malformed("directory parent link count underflow");
    }
    parent->attr.nlink--;
  }
  parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return txn_.HDel(key_.Directory(parent_ino), name);
}

utils::Status RedisMetaTxn::ReplaceEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                                         SwordFsInode *parent) {
  if (parent == nullptr) {
    return utils::Status::InvalidArgument("parent inode is null");
  }
  if (!parent->IsDir()) {
    return utils::Status::NotDirectory("parent is not a directory");
  }

  parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  SwordFsEntry entry{std::string(name), ModeToDt(child.attr.mode), child.ino};
  std::string value;
  auto status = entry.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  return txn_.HSet(key_.Directory(parent_ino), name, value);
}

utils::Status RedisMetaTxn::AdjustInodeCount(int64_t delta) {
  return txn_.IncrBy(key_.InodeCount(), delta);
}

utils::Status RedisMetaTxn::LookupChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  if (chunk == nullptr) {
    return utils::Status::InvalidArgument("chunk output is null");
  }

  std::string value;
  auto status = txn_.HGet(key_.Chunk(ino), std::to_string(idx), &value);
  if (!status.ok()) {
    return status;
  }
  return chunk->ParseFrom(value);
}

utils::Status RedisMetaTxn::ScanChunks(InodeID ino, std::vector<std::pair<std::string, SwordFsChunk>> &chunks) {
  chunks.clear();

  constexpr size_t kScanBatchSize = 128;
  uint64_t cursor = 0;
  do {
    std::vector<std::pair<std::string, std::string>> values;
    uint64_t next_cursor = 0;
    auto status = txn_.HScan(key_.Chunk(ino), cursor, kScanBatchSize, &values, &next_cursor);
    if (!status.ok()) {
      return status;
    }
    for (auto &[field, value] : values) {
      SwordFsChunk chunk;
      status = chunk.ParseFrom(value);
      if (!status.ok()) {
        return status;
      }
      if (field != std::to_string(chunk.index)) {
        return utils::Status::Malformed("persisted chunk descriptor does not match its canonical identity");
      }
      if (!chunk.IsValidForChunkSize(chunk_size_)) {
        return utils::Status::Malformed("persisted chunk descriptor does not match its canonical identity");
      }
      chunks.emplace_back(std::move(field), chunk);
    }
    cursor = next_cursor;
  } while (cursor != 0);

  return utils::Status::OK();
}

utils::Status RedisMetaTxn::QueuePendingDelete(InodeID ino, const SwordFsChunk &chunk) {
  const auto object_key = chunk::FormatChunkObjectKey(ino, chunk.index, chunk.revision);
  PendingDelete pending{.ino = ino, .chunk = ReclaimChunk{.descriptor = chunk, .key = object_key}};
  std::string encoded;
  auto status = pending.SerializeTo(&encoded);
  if (!status.ok()) {
    return status;
  }
  return txn_.HSet(key_.PendingDeletes(), object_key, encoded);
}

utils::Status RedisMetaTxn::RegisterPendingDeletes(InodeID ino, const std::vector<SwordFsChunk> &chunks) {
  for (const auto &chunk : chunks) {
    auto status = QueuePendingDelete(ino, chunk);
    if (!status.ok()) {
      return status;
    }
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                                        const SwordFsChunk &replacement, utils::Status &publication_result,
                                        std::optional<SwordFsChunk> &cleanup_candidate) {
  publication_result = utils::Status::OK();
  cleanup_candidate.reset();

  if (!replacement.IsValidForChunkSize(chunk_size_)) {
    return utils::Status::InvalidArgument("replacement chunk descriptor is invalid");
  }
  if (expected.has_value() && !expected->IsValidForChunkSize(chunk_size_)) {
    return utils::Status::InvalidArgument("expected chunk descriptor is invalid");
  }
  if (expected.has_value() && replacement.revision <= expected->revision) {
    return utils::Status::InvalidArgument("replacement revision must increase");
  }
  if (expected.has_value() &&
      (expected->index != replacement.index || expected->start_offset != replacement.start_offset)) {
    return utils::Status::InvalidArgument("replacement must preserve chunk index and start offset");
  }
  if (replacement.size > std::numeric_limits<uint64_t>::max() - replacement.start_offset) {
    return utils::Status::InvalidArgument("chunk end offset overflows");
  }

  auto reject_publication = [&](utils::Status rejection) -> utils::Status {
    // A known logical rejection proves this uploaded replacement is not the
    // authoritative descriptor. Cleanup registration happens after this
    // transaction has a known outcome and is intentionally best effort.
    cleanup_candidate = replacement;
    publication_result = std::move(rejection);
    return utils::Status::OK();
  };

  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (status.IsNotFound()) {
    return reject_publication(status);
  }
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return utils::Status::InvalidArgument("not a regular file");
  }

  SwordFsChunk current;
  status = LookupChunk(ino, replacement.index, &current);
  bool replacement_already_published = false;
  if (status.ok()) {
    if (!current.IsValidForChunkSize(chunk_size_)) {
      return utils::Status::Malformed("persisted chunk descriptor is invalid");
    }
    replacement_already_published = current == replacement;
    const bool current_matches_expected = expected.has_value() && current == *expected;
    if (!replacement_already_published && !current_matches_expected) {
      return reject_publication(utils::Status::AlreadyExists("chunk changed before publication at index " +
                                                             std::to_string(replacement.index)));
    }
    if (expected.has_value()) {
      cleanup_candidate = *expected;
    }
  } else if (status.IsNotFound()) {
    if (expected.has_value()) {
      return reject_publication(status);
    }
  } else {
    return status;
  }

  if (!replacement_already_published) {
    status = SetChunk(ino, replacement);
    if (!status.ok()) {
      return status;
    }
  }

  // Redis MULTI/EXEC does not roll back earlier commands when a later queued
  // command fails at execution time. Re-applying the inode side effects when
  // the replacement descriptor is already present lets a retry converge from
  // a partial EXEC where SetChunk succeeded but SetInode did not.
  const uint64_t chunk_end = replacement.start_offset + replacement.size;
  if (chunk_end > inode.attr.size) {
    inode.attr.size = chunk_end;
  }
  inode.attr.KillSUID();
  inode.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return SetInode(inode);
}

utils::Status RedisMetaTxn::SetChunk(InodeID ino, const SwordFsChunk &chunk) {
  std::string value;
  auto status = chunk.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  return txn_.HSet(key_.Chunk(ino), std::to_string(chunk.index), value);
}

utils::Status RedisMetaTxn::DeleteChunks(InodeID ino) {
  return txn_.Del(key_.Chunk(ino));
}

utils::Status RedisMetaTxn::TruncateChunks(InodeID ino, uint64_t old_size, uint64_t new_size,
                                           std::vector<SwordFsChunk> *detached_chunks) {
  if (new_size > old_size) {
    return utils::Status::OK();
  }
  if (chunk_size_ == 0) {
    return utils::Status::Internal("volume chunk size is not initialized");
  }

  // Drive truncate work from materialized metadata, never from logical file
  // length. ScanChunks WATCHes the chunk hash before any write is queued; a
  // concurrent chunk-map mutation therefore aborts EXEC and retries the whole
  // operation.
  std::vector<std::pair<std::string, SwordFsChunk>> chunks;
  auto status = ScanChunks(ino, chunks);
  if (!status.ok()) {
    return status;
  }

  for (auto &[field, chunk] : chunks) {
    if (chunk.start_offset >= new_size) {
      if (detached_chunks != nullptr) {
        detached_chunks->push_back(chunk);
      }
      status = txn_.HDel(key_.Chunk(ino), field);
      if (!status.ok()) {
        return status;
      }
      continue;
    }

    const uint64_t surviving_size = new_size - chunk.start_offset;
    if (chunk.size > surviving_size) {
      chunk.size = surviving_size;
      auto status = SetChunk(ino, chunk);
      if (!status.ok()) {
        return status;
      }
    }
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
