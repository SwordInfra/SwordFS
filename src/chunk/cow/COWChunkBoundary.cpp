// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/cow/COWChunkBoundary.hpp"

#include <cerrno>
#include <optional>

namespace swordfs::chunk::cow {

utils::Status internal::SanitizeCOWBoundaryImpl(metadata::cow::COWChunkMetadata &chunk_metadata,
                                                const metadata::RetainedChunkBoundary &boundary,
                                                const ChunkAttachmentProbeFn &probe_attachment) {
  if (!metadata::cow::internal::IsValidChunkID(boundary.chunk_id) || boundary.visible_prefix == 0 ||
      !probe_attachment) {
    return utils::Status::InvalidArgument("invalid COW boundary sanitation request");
  }

  for (;;) {
    metadata::cow::COWChunkHead current;
    auto status = chunk_metadata.GetHead(boundary.chunk_id, &current);
    if (status.IsNotFound()) {
      std::optional<metadata::ChunkID> attached;
      status = probe_attachment(boundary.index, &attached);
      if (!status.ok()) {
        return status;
      }
      if (attached.has_value() && *attached == boundary.chunk_id) {
        return utils::Status::Malformed("FileMetadata references a missing COW chunk head");
      }
      return utils::Status::OK();
    }
    if (!status.ok()) {
      return status;
    }
    if (current.size <= boundary.visible_prefix) {
      return utils::Status::OK();
    }

    auto replacement = current;
    replacement.size = boundary.visible_prefix;
    status = chunk_metadata.CompareExchangeHead(boundary.chunk_id, current, replacement);
    if (status.ok()) {
      return utils::Status::OK();
    }
    if (status.IsNotFound() || status.ToErrno() == EEXIST || status.IsOutcomeUnknown()) {
      continue;
    }
    return status;
  }
}

}  // namespace swordfs::chunk::cow
