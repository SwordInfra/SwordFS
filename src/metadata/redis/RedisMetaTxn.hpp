// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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
// policy; transaction-scoped reads stay here when their WATCH snapshot is part
// of the mutation's correctness contract.
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
  utils::Status SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out = nullptr,
                        std::vector<SwordFsChunk> *detached_chunks = nullptr);
  utils::Status Truncate(InodeID ino, uint64_t size, std::vector<SwordFsChunk> *detached_chunks = nullptr);
  utils::Status TouchInode(InodeID ino, SetAttrField fields);

  // ────────────────────────────────────────────────────────────────
  // Reclaim operations
  // ────────────────────────────────────────────────────────────────
  // Freeze the reclaim of |ino|. The transaction scans and WATCHes the
  // authoritative chunk hash itself before queuing any writes so the frozen
  // identities and live-metadata removal use one optimistic snapshot.
  //
  // On OK, |work| contains the frozen identities when the point of no return
  // was crossed/replayed. An empty optional is the normal non-reclaimable
  // outcome after stale orphan cleanup.
  utils::Status PrepareReclaim(InodeID ino, std::optional<ReclaimWork> &work);

  // Drop the frozen record of |ino|. Called only once every object of the
  // record has been deleted; missing records are not an error.
  utils::Status CompleteReclaim(InodeID ino);

  // Drop one immutable object from the truncate cleanup queue after physical
  // deletion. Missing entries are already complete.
  utils::Status CompletePendingDelete(std::string_view object_key);

  // Register immutable object identities as best-effort background cleanup
  // candidates. Queue membership is never delete authority: Reclaimer must
  // revalidate authoritative metadata before physical deletion.
  utils::Status RegisterPendingDeletes(InodeID ino, const std::vector<SwordFsChunk> &chunks);

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
  utils::Status UnlinkFile(InodeID parent_ino, std::string_view name, SwordFsInode *parent, SwordFsInode *child);
  utils::Status RemoveDirectory(InodeID parent_ino, std::string_view name, SwordFsInode *parent,
                                const SwordFsInode &child);
  utils::Status MoveEntry(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                          std::string_view new_name, SwordFsInode *old_parent, SwordFsInode *new_parent,
                          SwordFsInode *source, SwordFsInode *target, bool overwrite);
  utils::Status ExchangeEntries(InodeID old_parent_ino, std::string_view old_name, InodeID new_parent_ino,
                                std::string_view new_name, SwordFsInode *old_parent, SwordFsInode *new_parent,
                                SwordFsInode *source, SwordFsInode *target);
  utils::Status LinkExistingEntry(InodeID parent_ino, std::string_view name, SwordFsInode *parent, SwordFsInode *inode);

  // ────────────────────────────────────────────────────────────────
  // Chunk operations
  // ────────────────────────────────────────────────────────────────
  utils::Status CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected, const SwordFsChunk &replacement,
                            utils::Status &publication_result, std::optional<SwordFsChunk> &cleanup_candidate);

 private:
  utils::Status SetInode(const SwordFsInode &inode);
  utils::Status DeleteInode(InodeID ino);
  utils::Status AdjustNlink(SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);
  utils::Status LinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child, SwordFsInode *parent);
  utils::Status SetChunk(InodeID ino, const SwordFsChunk &chunk);
  utils::Status TruncateChunks(InodeID ino, uint64_t old_size, uint64_t new_size,
                               std::vector<SwordFsChunk> *detached_chunks);
  utils::Status DeleteChunks(InodeID ino);
  utils::Status IsDescendantOf(InodeID ancestor_ino, InodeID child_ino, bool *result);
  utils::Status DetachEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &target,
                            SwordFsInode *parent);
  utils::Status ReplaceEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                             SwordFsInode *parent);
  utils::Status AdjustInodeCount(int64_t delta);
  utils::Status LookupChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);
  utils::Status ScanChunks(InodeID ino, std::vector<std::pair<std::string, SwordFsChunk>> &chunks);
  utils::Status QueuePendingDelete(InodeID ino, const SwordFsChunk &chunk);

 private:
  RedisKvTxn &txn_;
  const redis::RedisKey &key_;
  uint64_t chunk_size_;
};

}  // namespace swordfs::metadata
