// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <functional>
#include <optional>
#include <utility>

#include "metadata/ChunkSizePlan.hpp"
#include "metadata/cow/COWChunkMetadata.hpp"
#include "utils/Status.hpp"

namespace swordfs::chunk::cow {

using ChunkAttachmentProbeFn = std::function<utils::Status(metadata::ChunkIndex, std::optional<metadata::ChunkID> *)>;

namespace internal {

utils::Status SanitizeCOWBoundaryImpl(metadata::cow::COWChunkMetadata &chunk_metadata,
                                      const metadata::RetainedChunkBoundary &boundary,
                                      const ChunkAttachmentProbeFn &probe_attachment);

}  // namespace internal

// Make a still-attached COW boundary safe for the visible prefix described by
// the mechanism-neutral FileMetadata plan. CAS conflicts are re-read rather
// than retried against a stale head. If the head disappears, FileMetadata is
// revalidated to distinguish a broken live reference from a detached/replaced
// ChunkID that is no longer authoritative.
//
// The attachment probe belongs to the FileMetadata adapter that #317 will
// activate. Keep it as a templated call boundary so #318 does not introduce an
// otherwise-unreferenced externally linked production symbol solely for staged
// verification.
template <typename ProbeAttachment>
utils::Status SanitizeCOWBoundary(metadata::cow::COWChunkMetadata &chunk_metadata,
                                  const metadata::RetainedChunkBoundary &boundary, ProbeAttachment &&probe_attachment) {
  ChunkAttachmentProbeFn probe(std::forward<ProbeAttachment>(probe_attachment));
  return internal::SanitizeCOWBoundaryImpl(chunk_metadata, boundary, probe);
}

}  // namespace swordfs::chunk::cow
