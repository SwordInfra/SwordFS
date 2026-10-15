// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisSliceChunkMetadata.hpp"

#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

utils::Status RedisSliceChunkMetadata::AllocateChunkID(ChunkID *) {
  utils::ExpectInFiberDomain();
  return utils::Status::NotSupported("Slice ChunkID allocation is not yet implemented");
}

utils::Status RedisSliceChunkMetadata::AllocateSliceID(chunk::slice::SliceID *) {
  utils::ExpectInFiberDomain();
  return utils::Status::NotSupported("SliceID allocation is not yet implemented");
}

utils::Status RedisSliceChunkMetadata::CreateIndex(ChunkID) {
  utils::ExpectInFiberDomain();
  return utils::Status::NotSupported("Slice index initialization is not yet implemented");
}

utils::Status RedisSliceChunkMetadata::ReadRecords(ChunkID, std::vector<chunk::slice::SliceRecord> *) {
  utils::ExpectInFiberDomain();
  return utils::Status::NotSupported("Slice index read is not yet implemented");
}

utils::Status RedisSliceChunkMetadata::AppendRecord(ChunkID, const chunk::slice::SliceRecord &) {
  utils::ExpectInFiberDomain();
  return utils::Status::NotSupported("Slice index append is not yet implemented");
}

}  // namespace swordfs::metadata
