// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Reclaim records — the durable, immutable object identities of an inode
// whose last directory entry is gone.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

/// One frozen chunk object identity: the authoritative descriptor that was
/// published for the inode, plus the immutable object key derived from it.
///
/// Delayed deletion must use |key| (equivalently the descriptor's revision)
/// verbatim. Rebuilding a key from live inode/index state at delete time is
/// exactly the race these durable cleanup records exist to close.
struct ReclaimChunk {
  SwordFsChunk descriptor;
  std::string key;

  bool operator==(const ReclaimChunk &) const = default;
};

/// The frozen work of one pending reclaim.
///
/// Published atomically with the removal of the live inode (see
/// IMetaEngine::PrepareReclaim) and removed only after every object in
/// |chunks| has been deleted from the data engine, or after the inode turns
/// out to be reclaimable no longer.
struct ReclaimWork {
  InodeID ino = 0;
  std::vector<ReclaimChunk> chunks;

  bool operator==(const ReclaimWork &) const = default;

  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

/// One immutable object detached from live chunk metadata and awaiting
/// physical deletion (currently produced by truncate).
///
/// The Redis Hash field is also |chunk.key|; persisting the full frozen
/// identity as the value lets recovery validate the field/key/descriptor
/// relationship before it is allowed to delete anything.
struct PendingDelete {
  InodeID ino = 0;
  ReclaimChunk chunk;

  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
