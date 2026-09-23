// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/IChunkOverwriteStrategy.hpp"

#include <memory>
#include <string>
#include <vector>

#include "chunk/Chunk.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"

namespace swordfs::chunk {
namespace {

class WholeObjectIndexParticipant final : public metadata::IChunkIndexParticipant {
 public:
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
    // The whole-object key is derived by this mechanism from the public
    // generation. It has no additional durable fragment records.
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
};

class WholeObjectStrategy final : public IChunkOverwriteStrategy {
 public:
  std::string_view name() const override {
    return "whole_object";
  }
  uint32_t index_format_version() const override {
    return 1;
  }
  std::shared_ptr<IChunkSession> OpenSession(metadata::InodeID ino, metadata::ChunkIndex index) const override {
    return std::make_shared<Chunk>(ino, index);
  }
  const metadata::IChunkIndexParticipant &index_participant() const override {
    static const WholeObjectIndexParticipant participant;
    return participant;
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
  utils::Status DeletePending(const metadata::PendingDelete &work, uint64_t chunk_size, metadata::IMetaEngine *meta,
                              storage::IDataEngine *data, bool *completed) const override {
    if (meta == nullptr || data == nullptr || completed == nullptr) {
      return utils::Status::InvalidArgument("whole-object pending delete requires engines and output");
    }
    *completed = false;
    WholeObjectRef ref;
    auto status = DecodeWholeObjectDelete(work, chunk_size, &ref);
    if (!status.ok()) {
      return status;
    }
    metadata::SwordFsChunk current;
    status = meta->FindChunk(ref.ino, ref.descriptor.index, &current);
    if (status.ok()) {
      if (current.revision == ref.descriptor.revision) {
        return utils::Status::OK();
      }
    } else if (!status.IsNotFound()) {
      return status;
    }
    status = data->Delete(ref.key);
    if (status.ok()) {
      *completed = true;
    }
    return status;
  }
  utils::Status DeleteFrozen(const metadata::ReclaimWork &work, uint64_t chunk_size, metadata::IMetaEngine *meta,
                             storage::IDataEngine *data) const override {
    if (meta == nullptr || data == nullptr) {
      return utils::Status::InvalidArgument("whole-object reclaim requires engines");
    }
    std::vector<WholeObjectRef> refs;
    auto status = DecodeWholeObjectReclaim(work, chunk_size, &refs);
    if (!status.ok()) {
      return status;
    }
    metadata::SwordFsInode inode;
    status = meta->GetInode(work.ino, &inode);
    if (status.ok()) {
      // Redis EXEC may partially apply. A frozen record alone does not prove
      // the live inode/head were removed, so never delete while it exists.
      return utils::Status::Busy("reclaim inode is still live");
    }
    if (!status.IsNotFound()) {
      return status;
    }
    size_t failed = 0;
    for (const auto &ref : refs) {
      status = data->Delete(ref.key);
      if (!status.ok()) {
        ++failed;
      }
    }
    if (failed != 0) {
      return utils::Status::IOError("whole-object reclaim left " + std::to_string(failed) + " object(s) undeleted");
    }
    return utils::Status::OK();
  }
};

}  // namespace

utils::Status CreateChunkOverwriteStrategy(std::string_view name, uint32_t index_format_version,
                                           std::unique_ptr<IChunkOverwriteStrategy> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk strategy output is null");
  }
  out->reset();
  if (name == "whole_object" && index_format_version == 1) {
    *out = std::make_unique<WholeObjectStrategy>();
    return utils::Status::OK();
  }
  return utils::Status::NotSupported("unsupported chunk overwrite strategy/index format: " + std::string(name) + "/" +
                                     std::to_string(index_format_version));
}

const IChunkOverwriteStrategy &DefaultChunkOverwriteStrategy() {
  static const WholeObjectStrategy strategy;
  return strategy;
}

}  // namespace swordfs::chunk
