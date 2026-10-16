// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <string>

#include "chunk/slice/SliceRecord.hpp"

namespace swordfs::chunk::slice {

// Volume-scoped physical identity, independent of the mutable Slice length.
inline std::string SliceObjectKey(SliceID id) {
  return "slice/" + std::to_string(id.Value());
}

}  // namespace swordfs::chunk::slice
