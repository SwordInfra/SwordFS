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
  virtual utils::Status Put(std::string_view hash, std::string_view field, std::string_view value) = 0;
  virtual utils::Status Erase(std::string_view hash, std::string_view field) = 0;
};

// Only the volume-selected mechanism interprets these bytes. The session
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

// Called before the public head/inode writes in the same backend transaction.
// Callbacks may prepare private index mutations and reject the whole mutation.
// Redis implementations must perform all reads before their first queued
// write, matching RedisKvTxn's WATCH/MULTI discipline. Redis EXEC can report
// per-command failures without rolling earlier commands back. A participant
// must not erase private data still reachable from the old public head, and
// a new head must refer only to already durable data or immutable private
// records whose creation cannot depend on a later public-head command.
class IChunkIndexParticipant {
 public:
  virtual ~IChunkIndexParticipant() = default;
  virtual utils::Status LoadPublished(IChunkIndexReader &reader, InodeID ino, const SwordFsChunk &head,
                                      std::string *private_snapshot) const = 0;
  virtual utils::Status Publish(IChunkIndexTxn &txn, InodeID ino, const std::optional<SwordFsChunk> &expected,
                                const SwordFsChunk &replacement, const ChunkPublishIntent &intent) const = 0;
  virtual utils::Status Truncate(IChunkIndexTxn &txn, InodeID ino,
                                 const std::vector<ChunkIndexChange> &changes) const = 0;
  virtual utils::Status PrepareReclaim(IChunkIndexTxn &txn, InodeID ino,
                                       const std::vector<SwordFsChunk> &heads) const = 0;
};

}  // namespace swordfs::metadata
