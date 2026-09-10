// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaTxn.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <utility>

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

utils::Status RedisMetaTxn::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
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

  if (size_changed) {
    status = TruncateChunks(ino, old_size, requested.size);
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

utils::Status RedisMetaTxn::Truncate(InodeID ino, uint64_t size) {
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (inode.attr.size == size) {
    return utils::Status::OK();
  }

  status = TruncateChunks(ino, inode.attr.size, size);
  if (!status.ok()) {
    return status;
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

utils::Status RedisMetaTxn::ReclaimInode(InodeID ino) {
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (status.IsNotFound()) {
    return utils::Status::OK();
  }
  if (!status.ok()) {
    return status;
  }
  if (inode.attr.nlink != 0) {
    return utils::Status::OK();
  }
  status = DeleteChunks(ino);
  if (!status.ok()) {
    return status;
  }
  return DeleteInode(ino);
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
                                       SwordFsInode *child, UnlinkResult *result) {
  if (parent == nullptr || child == nullptr) {
    return utils::Status::InvalidArgument("unlink inode is null");
  }
  if (parent->ino != parent_ino) {
    return utils::Status::InvalidArgument("entry parent mismatch");
  }
  if (child->IsDir()) {
    return utils::Status::InvalidArgument("cannot unlink directory");
  }

  auto status = AdjustNlink(child, -1, result != nullptr ? &result->post_nlink : nullptr);
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
  if (result != nullptr) {
    result->unlinked_ino = child->ino;
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
                                      SwordFsInode *source, SwordFsInode *target, bool overwrite,
                                      RenameResult *result) {
  if (result != nullptr) {
    *result = {};
  }
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
      UnlinkResult unlink_result;
      auto status = UnlinkFile(new_parent_ino, new_name, new_parent, target, &unlink_result);
      if (!status.ok()) {
        return status;
      }
      if (result != nullptr) {
        result->overwritten_ino = unlink_result.unlinked_ino;
        result->overwritten_post_nlink = unlink_result.post_nlink;
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
  status = AdjustNlink(inode, 1);
  if (!status.ok()) {
    return status;
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

utils::Status RedisMetaTxn::AddChunk(InodeID ino, const SwordFsChunk &chunk) {
  SwordFsInode inode;
  auto status = LookupInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  if (!inode.IsRegular()) {
    return utils::Status::InvalidArgument("not a regular file");
  }

  SwordFsChunk existing;
  status = LookupChunk(ino, chunk.index, &existing);
  if (status.ok()) {
    return utils::Status::AlreadyExists("chunk already exists at index " + std::to_string(chunk.index));
  }
  if (!status.IsNotFound()) {
    return status;
  }
  return SetChunk(ino, chunk);
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

utils::Status RedisMetaTxn::TruncateChunks(InodeID ino, uint64_t old_size, uint64_t new_size) {
  if (new_size >= old_size) {
    return utils::Status::OK();
  }
  if (chunk_size_ == 0) {
    return utils::Status::Internal("volume chunk size is not initialized");
  }
  if (new_size == 0) {
    return DeleteChunks(ino);
  }

  const ChunkIndex boundary_idx = static_cast<ChunkIndex>(new_size / chunk_size_);
  const uint64_t boundary_offset = new_size % chunk_size_;
  const ChunkIndex first_removed_idx = boundary_idx + (boundary_offset != 0 ? 1 : 0);
  const ChunkIndex old_chunk_count = static_cast<ChunkIndex>((old_size + chunk_size_ - 1) / chunk_size_);

  if (boundary_offset != 0) {
    SwordFsChunk chunk;
    auto status = LookupChunk(ino, boundary_idx, &chunk);
    if (status.ok()) {
      const uint64_t new_chunk_size = new_size - chunk.start_offset;
      if (chunk.size > new_chunk_size) {
        chunk.size = new_chunk_size;
        status = SetChunk(ino, chunk);
        if (!status.ok()) {
          return status;
        }
      }
    } else if (!status.IsNotFound()) {
      return status;
    }
  }

  for (ChunkIndex idx = first_removed_idx; idx < old_chunk_count; ++idx) {
    auto status = txn_.HDel(key_.Chunk(ino), std::to_string(idx));
    if (!status.ok()) {
      return status;
    }
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
