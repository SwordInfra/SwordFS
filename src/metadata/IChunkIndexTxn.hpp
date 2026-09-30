// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Mechanism-private hash names, fields, and encodings are opaque to metadata
// backends. A volume-selected participant defines their meaning. A hash may
// contain any number of records; this surface never assumes one entry or one
// physical reference for a logical chunk.
class IChunkIndexReader {
 public:
  virtual ~IChunkIndexReader() = default;
  virtual utils::Status Read(std::string_view hash, std::string_view field, std::string *value) = 0;
  virtual utils::Status Scan(std::string_view hash, std::vector<std::pair<std::string, std::string>> *values) = 0;
};

class IChunkIndexTxn : public IChunkIndexReader {
 public:
  ~IChunkIndexTxn() override = default;
  // Transitional raw record operations remain only for the current
  // common-authority ChunkMetadataBridge. Typed ChunkMetadata implementations
  // do not participate in this FileMetadata transaction.
  virtual utils::Status Put(std::string_view hash, std::string_view field, std::string_view value) = 0;
  virtual utils::Status Erase(std::string_view hash, std::string_view field) = 0;
};

// Only the volume-selected chunk-type implementation interprets these bytes. The session
// keeps its intent until publication has a known result, including retries.
struct ChunkPublishIntent {
  std::string payload;
};

// A consistent public head and mechanism-private read snapshot. The private
// bytes are decoded only by the selected chunk session, never by a backend.
struct ChunkView {
  SwordFsChunk head;
  std::string private_snapshot;
};

struct ChunkIndexChange {
  SwordFsChunk previous;
  std::optional<SwordFsChunk> current;
};

}  // namespace swordfs::metadata
