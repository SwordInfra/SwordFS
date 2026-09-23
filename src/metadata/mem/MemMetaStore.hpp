// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Combined inode + directory manager for the memory backend.
//
// Transaction model:
//   Transact() is the ONLY public entry point.  The callback receives a
//   MemMetaTxn handle (see MemMetaTxn.hpp) whose methods are the store's
//   primitive operations; the whole callback has one visibility boundary
//   (for the memory backend: a single critical section over mutex_). A
//   callback must return a primitive's status directly when it needs that
//   primitive's rejection to leave public metadata unchanged.
//
//   The transaction interface uses VALUE SEMANTICS: reads hand out
//   snapshot copies of SwordFsInode and writes go through explicit
//   by-ino semantic mutation primitives (SetAttr, Truncate,
//   TouchInode, SetSymlinkTarget, ...). No pointers into store-owned memory
//   ever escape a transaction, so callers do not have to replay metadata
//   bookkeeping around individual mutations.

#pragma once

#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <sys/stat.h>

#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include "metadata/mem/MemMetaTxn.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Entry.hpp"
#include "metadata/types/Inode.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::chunk {
class IChunkOverwriteStrategy;
}

namespace swordfs::metadata {

class MemMetaStore {
 public:
  MemMetaStore() : next_ino_(kRootInodeId + 1) {
    SwordFsAttr root_attr(kRootInodeId, S_IFDIR | 0755);
    inodes_[kRootInodeId] = std::make_unique<SwordFsInode>(kRootInodeId, root_attr, kRootInodeId);
    dirs_[kRootInodeId] = {};
  }
  ~MemMetaStore() = default;

  void BindChunkOverwriteStrategy(const chunk::IChunkOverwriteStrategy *strategy) {
    chunk_strategy_ = strategy;
  }
  void SetChunkSize(uint64_t chunk_size) {
    chunk_size_ = chunk_size;
  }

  // Run |f| as one atomic transaction.  The callback receives a
  // MemMetaTxn whose methods are the store's primitive operations; the
  // whole callback executes as a single atomic step with respect to all
  // other transactions (memory backend: while holding mutex_).
  //
  // This is the store's ONLY operation entry point — single operations
  // are simply single-primitive transactions.  It is also the seam
  // where a future KV/Redis backend maps the same callback shape onto
  // a real transaction.
  template <typename F>
  auto Transact(F &&f) {
    std::lock_guard<utils::FiberMutex> lock(mutex_);
    MemMetaTxn txn(this);
    using Result = std::invoke_result_t<F, MemMetaTxn &>;
    if constexpr (std::is_void_v<Result>) {
      std::forward<F>(f)(txn);
      txn.CommitPrivateIndex();
    } else {
      auto result = std::forward<F>(f)(txn);
      if constexpr (std::is_same_v<std::remove_cvref_t<Result>, Status>) {
        if (result.ok()) {
          txn.CommitPrivateIndex();
        }
      } else {
        txn.CommitPrivateIndex();
      }
      return result;
    }
  }

 private:
  // MemMetaTxn accesses the tables below directly; its lifetime is
  // exactly one critical section over mutex_.
  friend class MemMetaTxn;

  mutable utils::FiberMutex mutex_;
  std::atomic<InodeID> next_ino_;
  ChunkRevision next_chunk_revision_ = 1;
  const chunk::IChunkOverwriteStrategy *chunk_strategy_ = nullptr;
  uint64_t chunk_size_ = 0;

  folly::F14FastMap<InodeID, std::unique_ptr<SwordFsInode>> inodes_;
  folly::F14FastMap<InodeID, folly::F14FastMap<std::string, SwordFsInode *>> dirs_;

  // Chunk metadata: inode → (index → SwordFsChunk).
  folly::F14FastMap<InodeID, folly::F14FastMap<ChunkIndex, SwordFsChunk>> chunks_;
  // Private hash names and fields are supplied by the selected strategy.
  folly::F14FastMap<std::string, folly::F14FastMap<std::string, std::string>> private_chunk_index_;

  // Orphan candidates: inodes whose nlink dropped to zero. Published by the
  // mutation that dropped it (unlink, rename-overwrite) and dropped again by
  // Link revival, reclaim preparation, or as soon as the inode is found
  // unreclaimable. Mirrors the persistent backends' durable orphan marker
  // within the process lifetime.
  folly::F14FastSet<InodeID> orphans_;

  // Pending reclaims: frozen work of inodes that passed the reclaim point of
  // no return and whose objects have not all been deleted yet. This record —
  // not the live inode — is the authority for the delayed deletes.
  folly::F14FastMap<InodeID, ReclaimWork> pending_reclaims_;

  // Mechanism-private work made obsolete by truncate or publication. The
  // background Reclaimer asks the selected mechanism to validate/delete it;
  // opaque ids only provide stable acknowledgement and retry identity.
  folly::F14FastMap<std::string, PendingDelete> pending_deletes_;
};

}  // namespace swordfs::metadata
