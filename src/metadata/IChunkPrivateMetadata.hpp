// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <limits>

#include "metadata/types/Volume.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

// Stable backend-neutral identity for one mechanism-private sequence.
//
// Mechanisms define named aliases of ChunkPrivateSequenceTag beside their
// private record/ID types. The discriminator is therefore fixed at compile
// time; normal mechanism code never invents runtime string counter names.
class ChunkPrivateSequenceKey final {
 public:
  constexpr ChunkOverwriteMechanism mechanism() const {
    return mechanism_;
  }

  constexpr uint32_t discriminator() const {
    return discriminator_;
  }

 private:
  template <ChunkOverwriteMechanism Mechanism, uint32_t Discriminator>
  friend struct ChunkPrivateSequenceTag;

  constexpr ChunkPrivateSequenceKey(ChunkOverwriteMechanism mechanism, uint32_t discriminator)
      : mechanism_(mechanism), discriminator_(discriminator) {
  }

  ChunkOverwriteMechanism mechanism_;
  uint32_t discriminator_;
};

template <ChunkOverwriteMechanism Mechanism, uint32_t Discriminator>
struct ChunkPrivateSequenceTag final {
  static_assert(Mechanism == ChunkOverwriteMechanism::kWholeObject ||
                    Mechanism == ChunkOverwriteMechanism::kChunkSlice ||
                    Mechanism == ChunkOverwriteMechanism::kRedisCache,
                "mechanism-private sequence requires a known overwrite mechanism");
  static_assert(Discriminator != 0, "mechanism-private sequence discriminator 0 is reserved");
  static constexpr ChunkPrivateSequenceKey kKey{Mechanism, Discriminator};
};

inline constexpr uint64_t kMaxChunkPrivateSequenceValue = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());

inline utils::Status NextChunkPrivateSequenceValue(uint64_t current, uint64_t *next) {
  if (next == nullptr) {
    return utils::Status::InvalidArgument("private sequence output is null");
  }
  if (current >= kMaxChunkPrivateSequenceValue) {
    return utils::Status::IOError("private sequence exhausted");
  }
  *next = current + 1;
  return utils::Status::OK();
}

// Backend-neutral runtime capability root. Concrete mechanisms extend this
// interface with their own typed reader/store API next to their record types.
// The only schema-neutral operation is identity allocation because it has no
// record representation and intentionally occurs before publication.
class IChunkPrivateMetadataStore {
 public:
  virtual ~IChunkPrivateMetadataStore() = default;

  virtual utils::Status AllocateSequence(ChunkPrivateSequenceKey key, uint64_t *value) = 0;
};

// Root for transaction-scoped typed capabilities. Concrete mechanism
// transaction interfaces derive from this marker; backends keep their typed
// adapters inside the same transaction lifetime. IChunkIndexTxn derives from
// this only as a transitional bridge for the pre-#315 string surface.
class IChunkPrivateMetadataTxn {
 public:
  virtual ~IChunkPrivateMetadataTxn() = default;
};

}  // namespace swordfs::metadata
