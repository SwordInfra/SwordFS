// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisKvTxn;

// Metadata primitives bound to one optimistic Redis transaction.
//
// RedisMetaImpl composes these stateless primitives to implement POSIX
// semantics. Intermediate operation state stays in caller locals and is passed
// explicitly between primitives.
class RedisMetaTxn {
 public:
  RedisMetaTxn(RedisKvTxn &txn, const redis::RedisKey &key, uint64_t chunk_size);

  // ────────────────────────────────────────────────────────────────
  // Reads
  // ────────────────────────────────────────────────────────────────
  utils::Status LookupInode(InodeID ino, SwordFsInode *out);
  utils::Status LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out);
  utils::Status EntryExists(InodeID parent_ino, std::string_view name, bool *exists);
  utils::Status IsDirEmpty(InodeID ino, bool *empty);
  utils::Status IsDescendantOf(InodeID ancestor_ino, InodeID child_ino, bool *result);

  // ────────────────────────────────────────────────────────────────
  // Inode primitives
  // ────────────────────────────────────────────────────────────────
  utils::Status InsertInode(const SwordFsInode &inode);
  utils::Status SetInode(const SwordFsInode &inode);
  utils::Status DeleteInode(InodeID ino);
  utils::Status AdjustNlink(SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);

  // ────────────────────────────────────────────────────────────────
  // Directory-entry primitives
  // ────────────────────────────────────────────────────────────────
  utils::Status LinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child, SwordFsInode *parent);
  utils::Status UnlinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &target,
                            SwordFsInode *parent);
  utils::Status ReplaceEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                             SwordFsInode *parent);
  utils::Status DeleteDirectory(InodeID ino);
  utils::Status AdjustInodeCount(int64_t delta);

  // ────────────────────────────────────────────────────────────────
  // Chunk primitives
  // ────────────────────────────────────────────────────────────────
  utils::Status SetChunk(InodeID ino, const SwordFsChunk &chunk);
  utils::Status TruncateChunks(InodeID ino, uint64_t old_size, uint64_t new_size);
  utils::Status DeleteChunks(InodeID ino);

 private:
  utils::Status GetEntry(InodeID parent_ino, std::string_view name, SwordFsEntry *out);
  utils::Status LookupChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);

 private:
  RedisKvTxn &txn_;
  const redis::RedisKey &key_;
  uint64_t chunk_size_;
};

}  // namespace swordfs::metadata
