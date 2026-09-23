// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "metadata/IChunkIndexTxn.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace swordfs::chunk {
class IChunkOverwriteStrategy;
}

namespace swordfs::metadata {

class RedisKvTxn;

// Metadata operations bound to one optimistic Redis transaction.
//
// Public methods express complete metadata semantics that must stay atomic;
// low-level Redis metadata primitives remain private. RedisMetaImpl owns POSIX
// policy; transaction-scoped reads stay here when their WATCH snapshot is part
// of the mutation's correctness contract.
class RedisMetaTxn : public IChunkIndexTxn {
 public:
  RedisMetaTxn(RedisKvTxn &txn, const redis::RedisKey &key, uint64_t chunk_size,
               const chunk::IChunkOverwriteStrategy *strategy = nullptr);

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
                        std::vector<PendingDelete> *detached_chunks = nullptr);
  utils::Status Truncate(InodeID ino, uint64_t size, std::vector<PendingDelete> *detached_chunks = nullptr);
  utils::Status TouchInode(InodeID ino, SetAttrField fields);

  // ────────────────────────────────────────────────────────────────
  // Reclaim operations
  // ────────────────────────────────────────────────────────────────
  // Persist immutable reclaim work without removing live metadata. On OK,
  // |work| contains the frozen identities when reclaim is active/replayed.
  // An empty optional is the normal non-reclaimable outcome after stale
  // orphan cleanup.
  utils::Status FreezeReclaim(InodeID ino, std::optional<ReclaimWork> &work);

  // Finalize a previously frozen reclaim from a fresh watched snapshot.
  // Every state needed by the destructive transition is validated before the
  // first write is queued. Missing live inode means a previous finalization
  // already completed under the current protocol.
  utils::Status FinalizeReclaim(InodeID ino, const ReclaimWork &work);

  // Drop the frozen record of |ino|. Called only once every object of the
  // record has been deleted; missing records are not an error.
  utils::Status CompleteReclaim(InodeID ino);

  // Drop one immutable object from the truncate cleanup queue after physical
  // deletion. Missing entries are already complete.
  utils::Status CompletePendingDelete(std::string_view object_key);

  utils::Status Read(std::string_view hash, std::string_view field, std::string *value) override;
  utils::Status Scan(std::string_view hash, std::vector<std::pair<std::string, std::string>> *values) override;
  utils::Status Put(std::string_view hash, std::string_view field, std::string_view value) override;
  utils::Status Erase(std::string_view hash, std::string_view field) override;

  // Register immutable object identities as best-effort background cleanup
  // candidates. Queue membership is never delete authority: Reclaimer must
  // revalidate authoritative metadata before physical deletion.
  utils::Status RegisterPendingDeletes(const std::vector<PendingDelete> &work);

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
                            utils::Status &publication_result, std::optional<PendingDelete> &cleanup_candidate,
                            const ChunkPublishIntent &intent = {});
  utils::Status LoadChunkView(InodeID ino, ChunkIndex idx, ChunkView *out);

 private:
  utils::Status SetInode(const SwordFsInode &inode);
  utils::Status DeleteInode(InodeID ino);
  utils::Status AdjustNlink(SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);
  utils::Status LinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child, SwordFsInode *parent);
  utils::Status SetChunk(InodeID ino, const SwordFsChunk &chunk);
  utils::Status TruncateChunks(InodeID ino, uint64_t old_size, uint64_t new_size,
                               std::vector<PendingDelete> *detached_chunks);
  utils::Status DeleteChunks(InodeID ino);
  utils::Status ValidateInodeCount();
  utils::Status IsDescendantOf(InodeID ancestor_ino, InodeID child_ino, bool *result);
  utils::Status DetachEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &target,
                            SwordFsInode *parent);
  utils::Status ReplaceEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                             SwordFsInode *parent);
  utils::Status AdjustInodeCount(int64_t delta);
  utils::Status LookupChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk);
  utils::Status ScanChunks(InodeID ino, std::vector<std::pair<std::string, SwordFsChunk>> &chunks);
  utils::Status QueuePendingDelete(const PendingDelete &work);
  std::string PrivateHash(std::string_view hash) const;

 private:
  RedisKvTxn &txn_;
  const redis::RedisKey &key_;
  uint64_t chunk_size_;
  const chunk::IChunkOverwriteStrategy *strategy_;
};

}  // namespace swordfs::metadata
