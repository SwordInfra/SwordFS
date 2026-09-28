// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/mem/MemPrivateMetadataStore.hpp"

#include <mutex>

#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

utils::Status MemPrivateMetadataStore::AllocateSequenceImpl(uint32_t stable_id, uint64_t *value) {
  utils::ExpectInFiberDomain();
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  return internal::AllocatePrivateSequenceValue(&sequences_[stable_id], value);
}

}  // namespace swordfs::metadata
