// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "metadata/IChunkIndexTxn.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace folly {
class IOBuf;
}

namespace swordfs::metadata {
class IMetaEngine;
}
namespace swordfs::storage {
class IDataEngine;
}

namespace swordfs::chunk {

// One mount-local logical chunk. The implementation owns all private write
// representation and publication state; callers only know the logical range.
class IChunkSession {
 public:
  virtual ~IChunkSession() = default;
  virtual utils::Status Initialize() = 0;
  virtual utils::Status Write(off_t offset, const folly::IOBuf &data) = 0;
  virtual utils::Status Read(off_t offset, size_t len, folly::IOBuf *out) const = 0;
  virtual utils::Status Flush() = 0;
  virtual void Truncate(size_t size) = 0;
  virtual bool IsClean() const = 0;
  virtual bool Flushable() const = 0;
  virtual metadata::ChunkIndex index() const = 0;
  virtual off_t StartOffset() const = 0;
  virtual off_t DataEnd() const = 0;
};

// Constructed once from the format record at mount. All sessions for one
// volume therefore use the same mechanism and private index format.
class IChunkOverwriteStrategy {
 public:
  virtual ~IChunkOverwriteStrategy() = default;
  virtual std::string_view name() const = 0;
  virtual uint32_t index_format_version() const = 0;
  virtual std::shared_ptr<IChunkSession> OpenSession(metadata::InodeID ino, metadata::ChunkIndex index) const = 0;
  virtual const metadata::IChunkIndexParticipant &index_participant() const = 0;

  // Mechanism-owned cleanup encoding. Neither the common metadata engine nor
  // Reclaimer may infer a physical reference from a public chunk head.
  // Freeze while the same metadata transaction can still read the selected
  // mechanism's private index. A public head alone need not identify the
  // physical data that must eventually be removed.
  virtual utils::Status FreezePendingDelete(metadata::IChunkIndexTxn &txn, metadata::InodeID ino,
                                            const metadata::SwordFsChunk &head, uint64_t chunk_size,
                                            metadata::PendingDelete *out) const = 0;
  // A rejected candidate need not have a public head or durable private
  // index. Its session-owned intent alone must identify uploaded data for
  // best-effort cleanup; this callback cannot read a metadata transaction.
  virtual utils::Status FreezeRejectedPublication(metadata::InodeID ino, const metadata::SwordFsChunk &replacement,
                                                  const metadata::ChunkPublishIntent &intent, uint64_t chunk_size,
                                                  metadata::PendingDelete *out) const = 0;
  virtual utils::Status FreezeReclaim(metadata::IChunkIndexTxn &txn, metadata::InodeID ino,
                                      const std::vector<metadata::SwordFsChunk> &heads, uint64_t chunk_size,
                                      metadata::ReclaimWork *out) const = 0;
  virtual utils::Status DeletePending(const metadata::PendingDelete &work, uint64_t chunk_size,
                                      metadata::IMetaEngine *meta, storage::IDataEngine *data,
                                      bool *completed) const = 0;
  virtual utils::Status DeleteFrozen(const metadata::ReclaimWork &work, uint64_t chunk_size,
                                     metadata::IMetaEngine *meta, storage::IDataEngine *data) const = 0;
};

utils::Status CreateChunkOverwriteStrategy(std::string_view name, uint32_t index_format_version,
                                           std::unique_ptr<IChunkOverwriteStrategy> *out);
const IChunkOverwriteStrategy &DefaultChunkOverwriteStrategy();

}  // namespace swordfs::chunk
