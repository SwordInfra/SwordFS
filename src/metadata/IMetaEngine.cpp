// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/IMetaEngine.hpp"
#include "metadata/mem/MemMetaImpl.hpp"

namespace swordfs::metadata {

Limits IMetaEngine::GetLimits(std::string_view meta_url) {
  if (IsMemoryMode(meta_url)) {
    return MemMetaImpl::GetLimits();
  }
  return Limits{};
}

}  // namespace swordfs::metadata
