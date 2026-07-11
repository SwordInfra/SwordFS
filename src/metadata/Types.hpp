// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS metadata types — shared data structures used across the metadata
// subsystem.

#pragma once

#include <cstdint>
#include <string>

namespace swordfs::metadata {

using InodeID = uint64_t;

struct DirEntry {
  std::string name;
  InodeID ino;
  uint32_t type;  // DT_DIR, DT_REG, DT_LNK, etc.
};

}  // namespace swordfs::metadata
