// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/container/F14Map.h>

#include <cstdint>

#include "metadata/IPrivateMetadata.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {

// Mechanism-scoped Memory capability. Concrete mechanism stores derive from
// this class and keep typed records in native containers; this base owns only
// the common private sequence semantics established by #315.
class MemPrivateMetadataStore : public IMechanismPrivateStore {
 public:
  explicit MemPrivateMetadataStore(ChunkType mechanism) : mechanism_(mechanism) {
  }

  ChunkType mechanism() const override {
    return mechanism_;
  }

 private:
  utils::Status AllocateSequenceImpl(uint32_t stable_id, uint64_t *value) override;

 private:
  ChunkType mechanism_;
  utils::FiberMutex mutex_;
  folly::F14FastMap<uint32_t, uint64_t> sequences_;
};

}  // namespace swordfs::metadata
