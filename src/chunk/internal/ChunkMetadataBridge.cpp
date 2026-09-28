// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/internal/ChunkMetadataBridge.hpp"

#include <memory>
#include <string>
#include <utility>

#include "chunk/WholeObjectCleanup.hpp"

namespace swordfs::chunk::internal {
namespace {

class WholeObjectChunkMetadataBridge final : public ChunkMetadataBridge {
 public:
  explicit WholeObjectChunkMetadataBridge(metadata::MechanismPrivateStorePtr private_metadata)
      : private_metadata_(std::move(private_metadata)) {
  }

  utils::Status LoadPublished(metadata::IChunkIndexReader &, metadata::InodeID, const metadata::SwordFsChunk &,
                              std::string *private_snapshot) const override {
    if (private_snapshot == nullptr) {
      return utils::Status::InvalidArgument("whole-object private snapshot output is null");
    }
    private_snapshot->clear();
    return utils::Status::OK();
  }

  utils::Status Publish(metadata::IChunkIndexTxn &, metadata::InodeID, const std::optional<metadata::SwordFsChunk> &,
                        const metadata::SwordFsChunk &, const metadata::ChunkPublishIntent &intent) const override {
    if (!intent.payload.empty()) {
      return utils::Status::InvalidArgument("whole-object publication intent must be empty");
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
    return FreezeWholeObjectDelete(ino, head, chunk_size, out);
  }

  utils::Status FreezeRejectedPublication(metadata::InodeID ino, const metadata::SwordFsChunk &replacement,
                                          const metadata::ChunkPublishIntent &intent, uint64_t chunk_size,
                                          metadata::PendingDelete *out) const override {
    if (!intent.payload.empty()) {
      return utils::Status::InvalidArgument("whole-object publication intent must be empty");
    }
    return FreezeWholeObjectDelete(ino, replacement, chunk_size, out);
  }

  utils::Status FreezeReclaim(metadata::IChunkIndexTxn &, metadata::InodeID ino,
                              const std::vector<metadata::SwordFsChunk> &heads, uint64_t chunk_size,
                              metadata::ReclaimWork *out) const override {
    return FreezeWholeObjectReclaim(ino, heads, chunk_size, out);
  }

 private:
  // #316 replaces this retained generic capability with the typed
  // whole-object store; keeping it here preserves the mount-lifetime ownership
  // boundary established by #315 without leaking it back to common callers.
  metadata::MechanismPrivateStorePtr private_metadata_;
};

}  // namespace

utils::Status CreateChunkMetadataBridge(metadata::ChunkOverwriteMechanism mechanism,
                                        metadata::MechanismPrivateStorePtr private_metadata,
                                        std::unique_ptr<ChunkMetadataBridge> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk metadata bridge output is null");
  }
  out->reset();
  if (private_metadata == nullptr || private_metadata->mechanism() != mechanism) {
    return utils::Status::InvalidArgument("chunk metadata bridge private-store mechanism mismatch");
  }
  if (mechanism == metadata::ChunkOverwriteMechanism::kWholeObject) {
    *out = std::make_unique<WholeObjectChunkMetadataBridge>(std::move(private_metadata));
    return utils::Status::OK();
  }
  const auto name = metadata::ChunkOverwriteMechanismName(mechanism);
  return utils::Status::NotSupported("unsupported chunk metadata bridge mechanism: " + std::string(name));
}

}  // namespace swordfs::chunk::internal
