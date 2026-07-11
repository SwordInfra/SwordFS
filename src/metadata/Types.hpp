// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS metadata types — shared data structures used across the metadata
// subsystem.

#pragma once

#include <folly/container/F14Map.h>
#include <sys/stat.h>

#include <cstdint>
#include <string>

namespace swordfs::metadata {

// forward declaration
struct SwordFsInode;

using InodeID = uint64_t;
using InodeTable = folly::F14FastMap<InodeID, SwordFsInode*>;
using EntryTable = folly::F14FastMap<std::string, InodeID>;

struct SwordFsEntry {
  std::string name;
  // DT_DIR, DT_REG, DT_LNK, etc.
  uint32_t type;
  InodeID ino;
};

struct SwordFsInode {
  InodeID ino;
  struct stat attr;
  uint64_t nlookup = 0;  // reserved for future forget support
};

}  // namespace swordfs::metadata
