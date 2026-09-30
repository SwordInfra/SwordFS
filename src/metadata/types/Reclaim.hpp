// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Optional maintenance handoff after logical reclaim. Only the selected chunk
// type implementation may decode payload; the common metadata engine merely
// persists it. This record is never reclaim/reachability authority and may be
// lost after the FileMetadata point of no return at the cost of leaked storage.
struct ReclaimWork {
  InodeID ino = 0;
  std::string payload;

  bool operator==(const ReclaimWork &) const = default;
  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

// Best-effort cleanup candidate. `id` is an opaque queue identity, not a
// physical object key or permission to delete. The chunk-type implementation
// revalidates reachability before deleting and the common worker acknowledges
// by id.
struct PendingDelete {
  std::string id;
  std::string payload;

  bool operator==(const PendingDelete &) const = default;
  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
