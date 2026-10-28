// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "metadata/ChunkSizePlan.hpp"
#include "metadata/redis/RedisKey.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace swordfs::chunk::internal {
class ChunkCleanupWorkEncoder;
}

namespace swordfs::metadata {

class RedisKvTxn;
struct FileChunkSnapshot;
struct FileMappingSnapshot;
struct ChunkSizeCommitResult;

// Metadata operations bound to one optimistic Redis transaction.
//
// Public methods express complete metadata semantics that must stay atomic;
// low-level Redis metadata primitives remain private. RedisMetaImpl owns POSIX
// policy; transaction-scoped reads stay here when their WATCH snapshot is part
// of the mutation's correctness contract.
class RedisMetaTxn {
 public:
  RedisMetaTxn(RedisKvTxn &txn, const redis::RedisKey &key, uint64_t chunk_size,
               const chunk::internal::ChunkCleanupWorkEncoder *encoder);

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
  utils::Status SetInodeFlags(InodeID ino, InodeFlag inode_flags, SwordFsInode *out = nullptr);
  utils::Status SetXAttr(InodeID ino, std::string_view name, std::string_view value, XAttrSetMode mode);
  utils::Status RemoveXAttr(InodeID ino, std::string_view name);
  utils::Status TouchInode(InodeID ino, SetAttrField fields);

  // ────────────────────────────────────────────────────────────────
  // Reclaim operations
  // ────────────────────────────────────────────────────────────────
  // Prepare reclaim of |ino|. The transaction scans and WATCHes the
  // authoritative chunk hash itself before queuing any writes. It makes the
  // inode non-revivable before deleting chunk mappings; cleanup handoff is
  // optional maintenance queued only after that correctness transition.
  //
  // On OK, |work| contains cleanup identities produced by this fresh reclaim
  // attempt. An empty optional is the normal already-reclaimed,
  // non-reclaimable, or revived outcome.
  utils::Status PrepareReclaim(InodeID ino, std::optional<ReclaimWork> &work);

  // Drop optional cleanup maintenance for |ino|. Missing records are not an
  // error.
  utils::Status CompleteReclaim(InodeID ino);

  // Drop one immutable object from the truncate cleanup queue after physical
  // deletion. Missing entries are already complete.
  utils::Status CompletePendingDelete(std::string_view object_key);

  // Register immutable object identities as best-effort background cleanup
  // candidates. Queue membership is never delete authority: private chunk GC must
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
  utils::Status ReadFileMappingSnapshot(InodeID ino, FileMappingSnapshot *out);
  utils::Status ProbeAttachment(InodeID ino, ChunkIndex index, std::optional<ChunkID> *out);
  utils::Status AttachPrepared(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                               const FileSizePrecondition &expected);
  utils::Status FinalizeAttachedWrite(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                                      const FileSizePrecondition &expected);
  utils::Status CommitShrink(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested, SetAttrField fields,
                             ChunkSizeCommitResult *out);
  utils::Status CommitGrow(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested, SetAttrField fields,
                           ChunkSizeCommitResult *out);

 private:
  utils::Status CommitSizeChange(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                 SetAttrField fields, bool is_shrink, ChunkSizeCommitResult *out);
  utils::Status SetInode(const SwordFsInode &inode);
  utils::Status DeleteInode(InodeID ino);
  utils::Status AdjustNlink(SwordFsInode *inode, int delta, uint64_t *nlink = nullptr);
  utils::Status LinkEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child, SwordFsInode *parent);
  utils::Status IsDescendantOf(InodeID ancestor_ino, InodeID child_ino, bool *result);
  utils::Status DetachEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &target,
                            SwordFsInode *parent);
  utils::Status ReplaceEntry(InodeID parent_ino, std::string_view name, const SwordFsInode &child,
                             SwordFsInode *parent);
  utils::Status AdjustInodeCount(int64_t delta);
  utils::Status LookupAttachment(InodeID ino, ChunkIndex index, std::optional<ChunkID> *out);
  utils::Status QueuePendingDelete(const PendingDelete &work);

 private:
  RedisKvTxn &txn_;
  const redis::RedisKey &key_;
  uint64_t chunk_size_;
  const chunk::internal::ChunkCleanupWorkEncoder *cleanup_work_encoder_;
};

}  // namespace swordfs::metadata
