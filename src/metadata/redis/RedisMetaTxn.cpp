// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaTxn.hpp"

#include <dirent.h>
#include <folly/container/F14Set.h>
#include <sys/stat.h>

#include <string>
#include <utility>

#include "metadata/Utils.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"

namespace swordfs::metadata {

RedisMetaTxn::RedisMetaTxn(RedisMetaClient &raw, const redis::RedisKey &key, const uint64_t &chunk_size)
    : raw_(raw), key_(key), chunk_size_(chunk_size) {
}

utils::Status RedisMetaTxn::Transact(const TransactFn &callback) {
  return raw_.Transact([&](RedisKvTxn &kv_txn) {
    RedisMetaOpsContext ctx(kv_txn);
    return callback(ctx);
  });
}

utils::Status RedisMetaTxn::FormatVolume(const SwordFsVolume &config) {
  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = SwordFsAttr(kRootInodeId, S_IFDIR | 0755);
  std::string root_value;
  auto status = root.SerializeTo(&root_value);
  if (!status.ok()) {
    return status;
  }

  return Transact([&](RedisMetaOpsContext &ctx) {
    std::string existing;
    auto status = ctx.txn_.Get(key_.Format(), &existing);
    if (status.ok()) {
      return utils::Status::AlreadyExists("Redis metadata volume is already formatted");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    status = ctx.txn_.Set(key_.Format(), config.SerializeTo());
    if (!status.ok()) {
      return status;
    }
    status = ctx.txn_.Set(key_.NextIno(), std::to_string(kRootInodeId));
    if (!status.ok()) {
      return status;
    }
    status = ctx.txn_.Set(key_.InodeCount(), "1");
    if (!status.ok()) {
      return status;
    }
    return ctx.txn_.Set(key_.Inode(kRootInodeId), root_value);
  });
}

utils::Status RedisMetaTxn::LookupInode(RedisMetaOpsContext &ctx, InodeID ino, SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }

  std::string value;
  auto status = ctx.txn_.Get(key_.Inode(ino), &value);
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(value);
}

utils::Status RedisMetaTxn::GetEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                     SwordFsEntry *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("entry output is null");
  }

  std::string value;
  auto status = ctx.txn_.HGet(key_.Directory(parent_ino), name, &value);
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(value);
}

utils::Status RedisMetaTxn::LookupEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                        SwordFsInode *out) {
  SwordFsInode parent;
  auto status = LookupInode(ctx, parent_ino, &parent);
  if (!status.ok()) {
    return status;
  }
  if (!parent.IsDir()) {
    return utils::Status::NotDirectory("parent is not a directory");
  }

  SwordFsEntry entry;
  status = GetEntry(ctx, parent_ino, name, &entry);
  if (!status.ok()) {
    return status;
  }
  return LookupInode(ctx, entry.ino, out);
}

utils::Status RedisMetaTxn::EntryExists(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                        bool *exists) {
  if (exists == nullptr) {
    return utils::Status::InvalidArgument("entry existence output is null");
  }

  SwordFsEntry entry;
  auto status = GetEntry(ctx, parent_ino, name, &entry);
  if (status.IsNotFound()) {
    *exists = false;
    return utils::Status::OK();
  }
  if (!status.ok()) {
    return status;
  }
  *exists = true;
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::IsDirEmpty(RedisMetaOpsContext &ctx, InodeID ino, bool *empty) {
  if (empty == nullptr) {
    return utils::Status::InvalidArgument("directory empty output is null");
  }
  SwordFsInode dir;
  auto status = LookupInode(ctx, ino, &dir);
  if (!status.ok()) {
    return status;
  }
  if (!dir.IsDir()) {
    return utils::Status::NotDirectory("not a directory");
  }
  uint64_t length = 0;
  status = ctx.txn_.HLen(key_.Directory(ino), &length);
  if (!status.ok()) {
    return status;
  }
  *empty = length == 0;
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::IsDescendantOf(RedisMetaOpsContext &ctx, InodeID ancestor_ino, InodeID child_ino,
                                           bool *result) {
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
    auto status = LookupInode(ctx, current_ino, &inode);
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

utils::Status RedisMetaTxn::InsertInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode) {
  if (inode.ino == 0 || inode.attr.ino != inode.ino) {
    return utils::Status::InvalidArgument("invalid inode record");
  }

  SwordFsInode existing;
  auto status = LookupInode(ctx, inode.ino, &existing);
  if (status.ok()) {
    return utils::Status::AlreadyExists("inode already exists");
  }
  if (!status.IsNotFound()) {
    return status;
  }
  return SetInode(ctx, inode);
}

utils::Status RedisMetaTxn::SetInode(RedisMetaOpsContext &ctx, const SwordFsInode &inode) {
  if (inode.ino == 0 || inode.attr.ino != inode.ino) {
    return utils::Status::InvalidArgument("invalid inode record");
  }
  std::string value;
  auto status = inode.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  return ctx.txn_.Set(key_.Inode(inode.ino), value);
}

utils::Status RedisMetaTxn::DeleteInode(RedisMetaOpsContext &ctx, InodeID ino) {
  return ctx.txn_.Del(key_.Inode(ino));
}

utils::Status RedisMetaTxn::AdjustNlink(RedisMetaOpsContext &, SwordFsInode *inode, int delta, uint64_t *nlink) {
  if (inode == nullptr) {
    return utils::Status::InvalidArgument("inode is null");
  }
  if (delta < 0) {
    const uint64_t amount = static_cast<uint64_t>(-static_cast<int64_t>(delta));
    inode->attr.nlink = inode->attr.nlink > amount ? inode->attr.nlink - amount : 0;
  } else {
    inode->attr.nlink += static_cast<uint64_t>(delta);
  }
  if (nlink != nullptr) {
    *nlink = inode->attr.nlink;
  }
  return utils::Status::OK();
}

utils::Status RedisMetaTxn::LinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                      const SwordFsInode &child, SwordFsInode *parent) {
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
  return ctx.txn_.HSet(key_.Directory(parent_ino), name, value);
}

utils::Status RedisMetaTxn::UnlinkEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                        const SwordFsInode &target, SwordFsInode *parent) {
  if (parent == nullptr) {
    return utils::Status::InvalidArgument("parent inode is null");
  }
  if (!parent->IsDir()) {
    return utils::Status::NotDirectory("parent is not a directory");
  }

  if (target.IsDir() && parent->attr.nlink > 0) {
    parent->attr.nlink--;
  }
  parent->Touch(SetAttrField::kMtime | SetAttrField::kCtime);
  return ctx.txn_.HDel(key_.Directory(parent_ino), name);
}

utils::Status RedisMetaTxn::ReplaceEntry(RedisMetaOpsContext &ctx, InodeID parent_ino, std::string_view name,
                                         const SwordFsInode &child, SwordFsInode *parent) {
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
  return ctx.txn_.HSet(key_.Directory(parent_ino), name, value);
}

utils::Status RedisMetaTxn::DeleteDirectory(RedisMetaOpsContext &ctx, InodeID ino) {
  return ctx.txn_.Del(key_.Directory(ino));
}

utils::Status RedisMetaTxn::AdjustInodeCount(RedisMetaOpsContext &ctx, int64_t delta) {
  return ctx.txn_.IncrBy(key_.InodeCount(), delta);
}

utils::Status RedisMetaTxn::LookupChunk(RedisMetaOpsContext &ctx, InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  if (chunk == nullptr) {
    return utils::Status::InvalidArgument("chunk output is null");
  }

  std::string value;
  auto status = ctx.txn_.HGet(key_.Chunk(ino), std::to_string(idx), &value);
  if (!status.ok()) {
    return status;
  }
  return chunk->ParseFrom(value);
}

utils::Status RedisMetaTxn::SetChunk(RedisMetaOpsContext &ctx, InodeID ino, const SwordFsChunk &chunk) {
  std::string value;
  auto status = chunk.SerializeTo(&value);
  if (!status.ok()) {
    return status;
  }
  return ctx.txn_.HSet(key_.Chunk(ino), std::to_string(chunk.index), value);
}

utils::Status RedisMetaTxn::DeleteChunks(RedisMetaOpsContext &ctx, InodeID ino) {
  return ctx.txn_.Del(key_.Chunk(ino));
}

utils::Status RedisMetaTxn::TruncateChunks(RedisMetaOpsContext &ctx, InodeID ino, uint64_t old_size,
                                           uint64_t new_size) {
  if (new_size >= old_size) {
    return utils::Status::OK();
  }
  if (chunk_size_ == 0) {
    return utils::Status::Internal("volume chunk size is not initialized");
  }
  if (new_size == 0) {
    return DeleteChunks(ctx, ino);
  }

  const ChunkIndex boundary_idx = static_cast<ChunkIndex>(new_size / chunk_size_);
  const uint64_t boundary_offset = new_size % chunk_size_;
  const ChunkIndex first_removed_idx = boundary_idx + (boundary_offset != 0 ? 1 : 0);
  const ChunkIndex old_chunk_count = static_cast<ChunkIndex>((old_size + chunk_size_ - 1) / chunk_size_);

  if (boundary_offset != 0) {
    SwordFsChunk chunk;
    auto status = LookupChunk(ctx, ino, boundary_idx, &chunk);
    if (status.ok()) {
      const uint64_t new_chunk_size = new_size - chunk.start_offset;
      if (chunk.size > new_chunk_size) {
        chunk.size = new_chunk_size;
        status = SetChunk(ctx, ino, chunk);
        if (!status.ok()) {
          return status;
        }
      }
    } else if (!status.IsNotFound()) {
      return status;
    }
  }

  for (ChunkIndex idx = first_removed_idx; idx < old_chunk_count; ++idx) {
    auto status = ctx.txn_.HDel(key_.Chunk(ino), std::to_string(idx));
    if (!status.ok()) {
      return status;
    }
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
