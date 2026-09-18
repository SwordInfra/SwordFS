// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "metadata/redis/RedisKey.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

class RedisKvTxn;

// Metadata operations bound to one optimistic Redis transaction.
//
// Public methods express complete metadata semantics that must stay atomic;
// low-level Redis metadata primitives remain private. RedisMetaImpl owns POSIX
// policy and passes any already-read transaction snapshots explicitly so
// semantic operations do not repeat Redis reads.
class RedisMetaTxn {
 public:
  RedisMetaTxn(RedisKvTxn &txn, const redis::RedisKey &key, uint64_t chunk_size);

  // ────────────────────────────────────────────────────────────────
  // Reads
  // ────────────────────────────────────────────────────────────────
  utils::Status LookupInode(InodeID ino, SwordFsInode *out);
  utils::Status LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out);
  utils::Status LookupEntry(const SwordFsInode &parent, std::string_view name, SwordFsInode *out);

  // ────────────────────────────────────────────────────────────────
  // Inode operations
  // ────────────────────────────────────────────────────────────────
  utils::Status SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out = nullptr);
  utils::Status Truncate(InodeID ino, uint64_t size);
  utils::Status TouchInode(InodeID ino, SetAttrField fields);

  // ────────────────────────────────────────────────────────────────
  // Reclaim operations
  // ────────────────────────────────────────────────────────────────
  // Freeze the reclaim of |ino|, using the chunk descriptors the caller
  // scanned from the inode's chunk hash. The transaction re-validates that
  // scan (chunk count and a WATCH on the chunk hash) before it freezes, and
  // returns Busy when the chunk map changed in between so the caller can
  // re-scan. See IMetaEngine::PrepareReclaim for the full contract.
  //
  // On OK, |*frozen| reports whether the point of no return was crossed and
  // |*work| holds the frozen identities (possibly none). When |*frozen| is
  // false the inode was not reclaimable — already reclaimed, still linked, or
  // a directory — and any stale orphan marker was dropped by this same
  // transaction, so a concurrent unlink cannot publish a marker that this
  // call then deletes.
  utils::Status PrepareReclaim(InodeID ino, const std::vector<SwordFsChunk> &scanned, ReclaimWork &work, bool &frozen);

  // Drop the frozen record of |ino|. Called only once every object of the
  // record has been deleted; missing records are not an error.
  utils::Status CompleteReclaim(InodeID ino);

  // Drop |ino|'s orphan candidate marker, if any.
  utils::Status ClearOrphanMarker(InodeID ino);

  // ────────────────────────────────────────────────────────────────
  // Directory-entry operations
  // ────────────────────────────────────────────────────────────────
  // Atomically add a newly allocated inode to a directory. The caller owns
  // POSIX policy checks and inode construction; this operation owns the Redis
  // metadata invariants: dentry uniqueness, inode persistence, parent metadata
  // persistence and the global inode count.
  utils::Status AddEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child, SwordFsInode *parent);
  utils::Status UnlinkFile(InodeID parent_ino, std::string_view name, SwordFsInode *parent, SwordFsInode *child,
                           UnlinkResult *result);
  utils::Status RemoveDirectory(InodeID parent_ino, std::string_view name, SwordFsInode *parent,
                                const SwordFsInode &child);
  utils::Status MoveEntry(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                          std::string_view new_name, SwordFsInode *old_parent, SwordFsInode *new_parent,
                          SwordFsInode *source, SwordFsInode *target, bool overwrite, RenameResult *result);
  utils::Status ExchangeEntries(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                                std::string_view new_name, SwordFsInode *old_parent, SwordFsInode *new_parent,
                                SwordFsInode *source, SwordFsInode *target);
  utils::Status LinkExistingEntry(InodeID parent_ino, std::string_view name, SwordFsInode *parent, SwordFsInode *inode);

  // ────────────────────────────────────────────────────────────────
  // Chunk operations
  // ────────────────────────────────────────────────────────────────
  utils::Status CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement);

 private:
  utils::Status SetInode(const SwordFsInode &inode);
  utils::Status DeleteInode(InodeID ino);
  utils::Status AdjustNlink(SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);
  utils::Status LinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child, SwordFsInode *parent);
  utils::Status SetChunk(InodeID ino, const SwordFsChunk &chunk);
  utils::Status TruncateChunks(InodeID ino, uint64_t old_size, uint64_t new_size);
  utils::Status DeleteChunks(InodeID ino);
  utils::Status IsDescendantOf(InodeID ancestor_ino, InodeID child_ino, bool *result);
  utils::Status DetachEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &target,
                            SwordFsInode *parent);
  utils::Status ReplaceEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                             SwordFsInode *parent);
  utils::Status AdjustInodeCount(int64_t delta);
  utils::Status LookupChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);

 private:
  RedisKvTxn &txn_;
  const redis::RedisKey &key_;
  uint64_t chunk_size_;
};

}  // namespace swordfs::metadata
