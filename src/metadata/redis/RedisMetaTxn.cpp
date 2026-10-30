// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaTxn.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"
#include "metadata/ChunkSizePlan.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/InodePolicy.hpp"
#include "metadata/PosixAcl.hpp"
#include "metadata/Utils.hpp"
#include "metadata/redis/RedisKvTxn.hpp"

namespace swordfs::metadata {
namespace {

utils::Status ParseAttachedChunkID(std::string_view encoded, ChunkID *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("attached ChunkID output is null");
  }
  uint64_t value = 0;
  const auto [end, error] = std::from_chars(encoded.data(), encoded.data() + encoded.size(), value);
  if (error != std::errc{} || end != encoded.data() + encoded.size() || value == 0 || value > kMaxChunkIDValue) {
    return utils::Status::Malformed("FileMetadata contains an invalid ChunkID");
  }
  *out = ChunkID(value);
  return utils::Status::OK();
}

}  // namespace

RedisMetaTxn::RedisMetaTxn(RedisKvTxn &txn, const redis::RedisKey &key, uint64_t chunk_size,
                           const chunk::internal::ChunkCleanupWorkEncoder *encoder)
    : txn_(txn), key_(key), chunk_size_(chunk_size), cleanup_work_encoder_(encoder) {
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

utils::Status RedisMetaTxn::QueueAccessTimeChange(InodeID ino, AccessTimeChange change) {
  // A partial EXEC must never publish an explicit timestamp while retaining
  // an older overlay. For retirement, remove both sidecar and generation.
  auto status = txn_.Del(key_.AccessTime(ino));
  if (!status.ok()) {
    return status;
  }
  return change == AccessTimeChange::kExplicitSetAttr ? txn_.IncrBy(key_.AccessTimeEpoch(ino), 1)
                                                      : txn_.Del(key_.AccessTimeEpoch(ino));
}

utils::Status RedisMetaTxn::DeleteInode(InodeID ino) {
  auto status = txn_.Del(key_.Inode(ino));
  if (!status.ok()) {
    return status;
  }
  status = QueueAccessTimeChange(ino, AccessTimeChange::kRetireInode);
  return status.ok() ? AdjustInodeCount(-1) : status;
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

utils::Status RedisMetaTxn::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  // EOF mutations require typed boundary sanitation in FileReadWriter.
  if (HasSetAttrField(fields, SetAttrField::kSize)) {
    return utils::Status::InvalidArgument("size changes require FileReadWriter");
  }
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  status = CheckSetAttrPolicy(inode.attr, fields);
  if (!status.ok()) {
    return status;
  }

  SwordFsAttr attr = inode.attr;
  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    attr.mode = (attr.mode & S_IFMT) | (requested.mode & 07777);
  }
  if (HasSetAttrField(fields, SetAttrField::kUid)) {
    attr.uid = requested.uid;
  }
  if (HasSetAttrField(fields, SetAttrField::kGid)) {
    attr.gid = requested.gid;
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
  if (HasSetAttrField(fields, SetAttrField::kKillSuidGid)) {
    attr.ClearSetidForKillPriv();
  }
  if (!HasSetAttrField(fields, SetAttrField::kCtime)) {
    inode.attr = attr;
    inode.Touch(SetAttrField::kCtime);
    attr = inode.attr;
  }

  inode.attr = attr;
  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    status = SyncPosixAccessAclForMode(&inode, attr.mode);
    if (!status.ok()) {
      return status;
    }
  }
  if (HasSetAttrField(fields, SetAttrField::kAtime) || HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
    status = QueueAccessTimeChange(ino, AccessTimeChange::kExplicitSetAttr);
  }
  if (status.ok()) {
    status = SetInode(inode);
  }
  if (!status.ok()) {
    return status;
  }
  if (out != nullptr) {
    *out = inode;
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::SetInodeFlags(InodeID ino, InodeFlag inode_flags, SwordFsInode *out) {
  if (!HasOnlySupportedInodeFlags(inode_flags)) {
    return utils::Status::InvalidArgument("unsupported inode flags");
  }
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (inode.attr.inode_flags != inode_flags) {
    inode.attr.inode_flags = inode_flags;
    inode.Touch(SetAttrField::kCtime);
    status = SetInode(inode);
    if (!status.ok()) {
      return status;
    }
  }
  if (out != nullptr) {
    *out = inode;
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::SetXAttr(InodeID ino, std::string_view name, std::string_view value, XAttrSetMode mode) {
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(inode.attr, "setxattr");
  if (!status.ok()) {
    return status;
  }
  status = inode.SetXAttr(name, value, mode);
  if (!status.ok()) {
    return status;
  }
  return SetInode(inode);
}

utils::Status RedisMetaTxn::RemoveXAttr(InodeID ino, std::string_view name) {
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(inode.attr, "removexattr");
  if (!status.ok()) {
    return status;
  }
  status = inode.RemoveXAttr(name);
  if (!status.ok()) {
    return status;
  }
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

  // The inode is the serialization fence shared with Link. Cleanup records are
  // optional maintenance state and are deliberately not consulted as reclaim
  // authority.
  //
  // All reads happen before the first write. The live inode and the
  // complete chunk hash are WATCHed on the same Redis connection, so a
  // concurrent metadata mutation aborts EXEC and RedisMetaClient retries the
  // whole attempt from a fresh snapshot.
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  const bool has_inode = status.ok();
  if (!has_inode && !status.IsNotFound()) {
    return status;
  }

  if (!has_inode || inode.IsDir() || inode.attr.nlink != 0) {
    // The inode was already reclaimed, is a directory, or a concurrent Link
    // revived it. A maintenance record must never override that authoritative
    // state. For a live/unreclaimable inode, drop any stale cleanup handoff so
    // it cannot become future delete authority after an unrelated transition.
    status = ClearOrphanMarker(ino);
    if (!status.ok()) {
      return status;
    }
    if (has_inode) {
      return CompleteReclaim(ino);
    }
    return utils::Status::OK();
  }

  FileMappingSnapshot typed;
  if (inode.IsRegular()) {
    // Scan and WATCH the authoritative map under the same transaction as the
    // inode's last-link fence; a competing attachment must abort EXEC.
    status = ReadFileMappingSnapshot(ino, &typed);
    if (!status.ok()) {
      return status;
    }
  }

  std::string serialized;
  if (cleanup_work_encoder_ == nullptr) {
    return utils::Status::Internal("cleanup work encoder is not bound");
  }
  ReclaimWork pending;
  status = cleanup_work_encoder_->EncodeDetachedReclaim(ino, typed.mappings, &pending);
  if (!status.ok()) {
    return status;
  }
  status = pending.SerializeTo(&serialized);
  if (!status.ok()) {
    return status;
  }

  // Critical destructive ordering: make the inode non-revivable before
  // detaching its mapping state. Both operations use type-insensitive DEL, so
  // a later auxiliary command failure cannot leave a revivable inode whose
  // mappings were already destroyed.
  status = txn_.Del(key_.Inode(ino));
  if (!status.ok()) {
    return status;
  }
  status = txn_.Del(key_.ChunkRefs(ino));
  if (status.ok()) {
    status = QueueAccessTimeChange(ino, AccessTimeChange::kRetireInode);
  }
  if (!status.ok()) {
    return status;
  }

  // Everything below is maintenance. Losing it after the point of no return
  // can leak cleanup work but cannot make the inode or detached state live.
  status = ClearOrphanMarker(ino);
  if (!status.ok()) {
    return status;
  }
  status = txn_.HSet(key_.Reclaims(), std::to_string(ino), serialized);
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
  auto status = CheckDirectoryAdditionPolicy(parent->attr);
  if (!status.ok()) {
    return status;
  }

  std::string existing_value;
  status = txn_.HGet(key_.Directory(parent_ino), name, &existing_value);
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

  auto status = CheckDirectoryRemovalPolicy(parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(child->attr, "unlink");
  if (!status.ok()) {
    return status;
  }

  status = AdjustNlink(child, -1);
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

  auto status = CheckDirectoryRemovalPolicy(parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(child.attr, "rmdir");
  if (!status.ok()) {
    return status;
  }

  uint64_t length = 0;
  status = txn_.HLen(key_.Directory(child.ino), &length);
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

  auto status = CheckDirectoryRemovalPolicy(old_parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckDirectoryAdditionPolicy(new_parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(source->attr, "rename");
  if (!status.ok()) {
    return status;
  }

  if (source->IsDir()) {
    bool cycle = false;
    status = IsDescendantOf(source->ino, new_parent_ino, &cycle);
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
    status = CheckDirectoryRemovalPolicy(new_parent->attr);
    if (!status.ok()) {
      return status;
    }
    status = CheckContentMetadataMutationPolicy(target->attr, "rename replacement");
    if (!status.ok()) {
      return status;
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

  status = DetachEntry(old_parent_ino, old_name, *source, old_parent);
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
  auto status = CheckDirectoryRemovalPolicy(old_parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckDirectoryRemovalPolicy(new_parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(source->attr, "rename exchange");
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(target->attr, "rename exchange");
  if (!status.ok()) {
    return status;
  }
  if (source->ino == target->ino) {
    return utils::Status::OK();
  }

  auto validate_directory_destination = [&](const SwordFsInode &inode, InodeID parent_ino) -> utils::Status {
    if (!inode.IsDir()) {
      return utils::Status::OK();
    }
    bool cycle = false;
    auto status = IsDescendantOf(inode.ino, parent_ino, &cycle);
    if (!status.ok()) {
      return status;
    }
    if (cycle) {
      return utils::Status::InvalidArgument("cannot exchange directory into its descendant");
    }
    return utils::Status::OK();
  };

  status = validate_directory_destination(*source, new_parent_ino);
  if (!status.ok()) {
    return status;
  }
  status = validate_directory_destination(*target, old_parent_ino);
  if (!status.ok()) {
    return status;
  }

  // An exchange preserves both names and both inodes. Across different
  // parents, only the net number of directory children changes each parent's
  // link count: target-dir minus source-dir for the old parent, with the
  // opposite delta for the new parent.
  if (old_parent != new_parent) {
    const int old_parent_delta = static_cast<int>(target->IsDir()) - static_cast<int>(source->IsDir());
    if (old_parent_delta != 0) {
      status = AdjustNlink(old_parent, old_parent_delta);
      if (!status.ok()) {
        return status;
      }
      status = AdjustNlink(new_parent, -old_parent_delta);
      if (!status.ok()) {
        return status;
      }
    }
  }

  status = ReplaceEntry(old_parent_ino, old_name, *target, old_parent);
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
  auto status = CheckDirectoryAdditionPolicy(parent->attr);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(inode->attr, "link");
  if (!status.ok()) {
    return status;
  }

  SwordFsInode existing;
  status = LookupEntry(*parent, name, &existing);
  if (status.ok()) {
    return utils::Status::AlreadyExists("entry already exists");
  }
  if (!status.IsNotFound()) {
    return status;
  }

  // Link and reclaim both reached this point from a watched read of the same
  // inode. Whichever transaction changes/deletes it first invalidates the
  // other's snapshot, so no shared reclaim-Hash fence is needed.

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

utils::Status RedisMetaTxn::QueuePendingDelete(const PendingDelete &pending) {
  std::string encoded;
  auto status = pending.SerializeTo(&encoded);
  if (!status.ok()) {
    return status;
  }
  return txn_.HSet(key_.PendingDeletes(), pending.id, encoded);
}

utils::Status RedisMetaTxn::RegisterPendingDeletes(const std::vector<PendingDelete> &work) {
  for (const auto &pending : work) {
    auto status = QueuePendingDelete(pending);
    if (!status.ok()) {
      return status;
    }
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::LookupAttachment(InodeID ino, ChunkIndex index, std::optional<ChunkID> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("attachment output is null");
  }
  out->reset();
  std::string encoded;
  auto status = txn_.HGet(key_.ChunkRefs(ino), std::to_string(index), &encoded);
  if (status.IsNotFound()) {
    return utils::Status::OK();
  }
  if (!status.ok()) {
    return status;
  }
  ChunkID chunk_id;
  status = ParseAttachedChunkID(encoded, &chunk_id);
  if (status.ok()) {
    *out = chunk_id;
  }
  return status;
}

utils::Status RedisMetaTxn::ReadFileMappingSnapshot(InodeID ino, FileMappingSnapshot *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("file mapping snapshot output is null");
  }
  FileMappingSnapshot result;
  auto status = LookupInode(ino, &result.inode);
  if (!status.ok()) {
    return status;
  }
  if (!result.inode.IsRegular()) {
    return utils::Status::InvalidArgument("not a regular file");
  }
  uint64_t cursor = 0;
  do {
    std::vector<std::pair<std::string, std::string>> page;
    uint64_t next_cursor = 0;
    status = txn_.HScan(key_.ChunkRefs(ino), cursor, 128, &page, &next_cursor);
    if (!status.ok()) {
      return status;
    }
    for (const auto &[index_field, id_value] : page) {
      ChunkIndex index = 0;
      const auto [end, error] = std::from_chars(index_field.data(), index_field.data() + index_field.size(), index);
      if (error != std::errc{} || end != index_field.data() + index_field.size()) {
        return utils::Status::Malformed("FileMetadata contains an invalid chunk index");
      }
      ChunkID chunk_id;
      status = ParseAttachedChunkID(id_value, &chunk_id);
      if (!status.ok()) {
        return status;
      }
      result.mappings.push_back({.index = index, .chunk_id = chunk_id});
    }
    cursor = next_cursor;
  } while (cursor != 0);
  std::sort(result.mappings.begin(), result.mappings.end(),
            [](const ChunkMapping &lhs, const ChunkMapping &rhs) { return lhs.index < rhs.index; });
  for (size_t i = 1; i < result.mappings.size(); ++i) {
    if (result.mappings[i - 1].index == result.mappings[i].index) {
      return utils::Status::Malformed("duplicate FileMetadata chunk index");
    }
  }
  *out = std::move(result);
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::ProbeAttachment(InodeID ino, ChunkIndex index, std::optional<ChunkID> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("attachment output is null");
  }
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  return LookupAttachment(ino, index, out);
}

utils::Status RedisMetaTxn::AttachPrepared(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                                           const FileSizePrecondition &expected) {
  auto status = ValidateFileChunkWrite(index, chunk_id, end, chunk_size_);
  if (!status.ok()) {
    return status;
  }
  status = ValidateFileSizePrecondition(expected, chunk_size_);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode inode;
  status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return utils::Status::InvalidArgument("not a regular file");
  }
  std::optional<ChunkID> current;
  status = LookupAttachment(ino, index, &current);
  if (!status.ok()) {
    return status;
  }
  status = CheckContentMetadataMutationPolicy(inode.attr, "attach chunk");
  if (!status.ok()) {
    return status;
  }
  if (inode.attr.size != expected.eof || current.has_value()) {
    return utils::Status::AlreadyExists("FileMetadata changed before chunk attachment");
  }
  if (expected.boundary.has_value()) {
    std::optional<ChunkID> observed;
    status = LookupAttachment(ino, expected.boundary->index, &observed);
    if (!status.ok()) {
      return status;
    }
    if (observed != expected.boundary->chunk_id) {
      return utils::Status::AlreadyExists("FileMetadata EOF boundary changed before attachment");
    }
  }
  inode.attr.size = std::max(inode.attr.size, end);
  inode.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  // Redis MULTI/EXEC does not roll back preceding commands when a later
  // command errors. Publish EOF first so a partial EXEC can expose only a
  // zero-filled hole, never a mapped COW head with stale inode attributes.
  status = SetInode(inode);
  if (!status.ok()) {
    return status;
  }
  return txn_.HSet(key_.ChunkRefs(ino), std::to_string(index), std::to_string(chunk_id.Value()));
}

utils::Status RedisMetaTxn::FinalizeAttachedWrite(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                                                  const FileSizePrecondition &expected) {
  auto status = ValidateFileChunkWrite(index, chunk_id, end, chunk_size_);
  if (!status.ok()) {
    return status;
  }
  status = ValidateFileSizePrecondition(expected, chunk_size_);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode inode;
  status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return utils::Status::InvalidArgument("not a regular file");
  }
  std::optional<ChunkID> current;
  status = LookupAttachment(ino, index, &current);
  if (!status.ok()) {
    return status;
  }
  if (current != chunk_id) {
    return utils::Status::AlreadyExists("FileMetadata attachment changed before finalization");
  }
  if (expected.boundary.has_value()) {
    std::optional<ChunkID> observed;
    status = LookupAttachment(ino, expected.boundary->index, &observed);
    if (!status.ok()) {
      return status;
    }
    if (observed != expected.boundary->chunk_id) {
      return utils::Status::AlreadyExists("FileMetadata EOF boundary changed before finalization");
    }
  }
  // EOF alone is not a publication identity: another mount may have
  // extended the file since this distinct SliceRecord was appended.
  // Returning success merely because EOF >= end would skip this write's
  // mtime/ctime update and potentially bypass a new immutable policy.
  // Force the caller to refresh FileMetadata without reappending the record.
  if (inode.attr.size != expected.eof) {
    return utils::Status::AlreadyExists("FileMetadata EOF changed before finalization");
  }
  status = CheckContentMetadataMutationPolicy(inode.attr, "finalize chunk write");
  if (!status.ok()) {
    return status;
  }
  inode.attr.size = std::max(inode.attr.size, end);
  inode.Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return SetInode(inode);
}

utils::Status RedisMetaTxn::CommitShrink(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                         SetAttrField fields, ChunkSizeCommitResult *out) {
  return CommitSizeChange(ino, plan, requested, fields, /*is_shrink=*/true, out);
}

utils::Status RedisMetaTxn::CommitGrow(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                       SetAttrField fields, ChunkSizeCommitResult *out) {
  return CommitSizeChange(ino, plan, requested, fields, /*is_shrink=*/false, out);
}

utils::Status RedisMetaTxn::CommitSizeChange(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                             SetAttrField fields, bool is_shrink, ChunkSizeCommitResult *out) {
  if (out == nullptr || !HasSetAttrField(fields, SetAttrField::kSize) || requested.size != plan.target_eof) {
    return utils::Status::InvalidArgument("typed size commit requires matching SetAttr size and result");
  }
  FileMappingSnapshot snapshot;
  auto status = ReadFileMappingSnapshot(ino, &snapshot);
  if (!status.ok()) {
    return status;
  }
  ChunkSizePlan current;
  status = ValidateSizeCommitPlan(plan, snapshot.inode.attr.size, chunk_size_, snapshot.mappings, is_shrink, &current);
  if (!status.ok()) {
    return status;
  }
  SwordFsInode updated;
  status = PrepareSetAttrMutation(snapshot.inode, requested, fields, &updated);
  if (!status.ok()) {
    return status;
  }

  ChunkSizeCommitResult result{.inode = updated, .boundary = current.boundary};
  ChunkSizeLayout old_layout;
  status = PlanChunkSizeLayout(snapshot.inode.attr.size, chunk_size_, &old_layout);
  if (!status.ok()) {
    return status;
  }
  ChunkSizeLayout new_layout;
  status = PlanChunkSizeLayout(plan.target_eof, chunk_size_, &new_layout);
  if (!status.ok()) {
    return status;
  }
  for (const auto &mapping : snapshot.mappings) {
    if ((is_shrink && new_layout.ShouldDetach(mapping.index)) ||
        (!is_shrink && old_layout.ShouldDetach(mapping.index))) {
      result.detached.push_back(mapping);
    }
  }

  if (!result.detached.empty()) {
    if (cleanup_work_encoder_ == nullptr) {
      return utils::Status::Internal("cleanup work encoder is not bound");
    }
    // Enqueue immutable identities in the same watched Redis transaction as
    // detach/EOF. A partial EXEC remains safe: GC must recheck reachability.
    for (const auto &mapping : result.detached) {
      PendingDelete pending;
      status = cleanup_work_encoder_->EncodeDetachedDelete(ino, mapping, &pending);
      if (!status.ok()) {
        return status;
      }
      status = QueuePendingDelete(pending);
      if (!status.ok()) {
        return status;
      }
    }
  }

  const auto detach = [&]() -> utils::Status {
    for (const auto &mapping : result.detached) {
      auto detached_status = txn_.HDel(key_.ChunkRefs(ino), std::to_string(mapping.index));
      if (!detached_status.ok()) {
        return detached_status;
      }
    }
    return utils::Status::OK();
  };
  // Queue shrink's new EOF first: a partial Redis EXEC cannot make the old
  // EOF expose missing mappings. For grow, remove any stale detached mappings
  // left outside the old EOF *before* publishing a larger EOF.
  // The sidecar is cleared and stale readers fenced before the new inode
  // is published, including a size-changing explicit atime SetAttr.
  if (HasSetAttrField(fields, SetAttrField::kAtime) || HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
    status = QueueAccessTimeChange(ino, AccessTimeChange::kExplicitSetAttr);
  }
  if (status.ok() && is_shrink) {
    status = SetInode(updated);
    if (status.ok()) {
      status = detach();
    }
  } else if (status.ok()) {
    status = detach();
    if (status.ok()) {
      status = SetInode(updated);
    }
  }
  if (!status.ok()) {
    return status;
  }
  *out = std::move(result);
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
