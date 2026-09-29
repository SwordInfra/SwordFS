// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/cow/COWChunkMetadataBridge.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "chunk/cow/COWCleanup.hpp"

namespace swordfs::chunk::cow {
namespace {

class COWChunkMetadataBridge final : public internal::ChunkMetadataBridge {
 public:
  explicit COWChunkMetadataBridge(metadata::MechanismPrivateStorePtr private_metadata)
      : private_metadata_(std::move(private_metadata)) {
  }

  utils::Status LoadPublished(metadata::IChunkIndexReader &, metadata::InodeID, const metadata::SwordFsChunk &,
                              std::string *private_snapshot) const override {
    if (private_snapshot == nullptr) {
      return utils::Status::InvalidArgument("COW private snapshot output is null");
    }
    private_snapshot->clear();
    return utils::Status::OK();
  }

  utils::Status Publish(metadata::IChunkIndexTxn &, metadata::InodeID, const std::optional<metadata::SwordFsChunk> &,
                        const metadata::SwordFsChunk &, const metadata::ChunkPublishIntent &intent) const override {
    if (!intent.payload.empty()) {
      return utils::Status::InvalidArgument("COW publication intent must be empty");
    }
    return utils::Status::OK();
  }

  utils::Status Truncate(metadata::IChunkIndexTxn &, metadata::InodeID,
                         const std::vector<metadata::ChunkIndexChange> &) const override {
    return utils::Status::OK();
  }

  utils::Status PrepareReclaim(metadata::IChunkIndexTxn &, metadata::InodeID,
                               const std::vector<metadata::SwordFsChunk> &) const override {
    return utils::Status::OK();
  }

  utils::Status FreezePendingDelete(metadata::IChunkIndexTxn &, metadata::InodeID ino,
                                    const metadata::SwordFsChunk &head, uint64_t chunk_size,
                                    metadata::PendingDelete *out) const override {
    return FreezeCOWDelete(ino, head, chunk_size, out);
  }

  utils::Status FreezeRejectedPublication(metadata::InodeID ino, const metadata::SwordFsChunk &replacement,
                                          const metadata::ChunkPublishIntent &intent, uint64_t chunk_size,
                                          metadata::PendingDelete *out) const override {
    if (!intent.payload.empty()) {
      return utils::Status::InvalidArgument("COW publication intent must be empty");
    }
    return FreezeCOWDelete(ino, replacement, chunk_size, out);
  }

  utils::Status FreezeReclaim(metadata::IChunkIndexTxn &, metadata::InodeID ino,
                              const std::vector<metadata::SwordFsChunk> &heads, uint64_t chunk_size,
                              metadata::ReclaimWork *out) const override {
    return FreezeCOWReclaim(ino, heads, chunk_size, out);
  }

 private:
  // #316 replaces this retained generic capability with the typed COW store;
  // keeping it here preserves the mount-lifetime ownership boundary from #315.
  metadata::MechanismPrivateStorePtr private_metadata_;
};

}  // namespace

utils::Status CreateCOWChunkMetadataBridge(metadata::MechanismPrivateStorePtr private_metadata,
                                           std::unique_ptr<internal::ChunkMetadataBridge> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("COW chunk metadata bridge output is null");
  }
  *out = std::make_unique<COWChunkMetadataBridge>(std::move(private_metadata));
  return utils::Status::OK();
}

}  // namespace swordfs::chunk::cow
