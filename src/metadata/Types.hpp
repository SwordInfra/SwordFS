// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// SwordFS metadata types — shared data structures used across the metadata
// subsystem.

#pragma once

#include <folly/container/F14Map.h>
#include <sys/stat.h>

#include <cstdint>
#include <ctime>
#include <string>

namespace swordfs::metadata {

// forward declaration
struct SwordFsInode;

using InodeID = uint64_t;
using InodeTable = folly::F14FastMap<InodeID, SwordFsInode*>;
using EntryTable = folly::F14FastMap<std::string, SwordFsInode*>;

struct SwordFsEntry {
  std::string name;
  // DT_DIR, DT_REG, DT_LNK, etc.
  uint32_t type;
  InodeID ino;
};

enum TimeField : uint8_t {
  kAtime = 1 << 0,
  kMtime = 1 << 1,
  kCtime = 1 << 2,
};

struct SwordFsInode {
  InodeID ino;
  struct stat attr;
  uint64_t nlookup = 0;  // reserved for future forget support

  void Touch(uint8_t fields) {
    time_t now = ::time(nullptr);
    if (fields & kAtime) attr.st_atime = now;
    if (fields & kMtime) attr.st_mtime = now;
    if (fields & kCtime) attr.st_ctime = now;
  }

  bool IsDir() const { return S_ISDIR(attr.st_mode); }
};

}  // namespace swordfs::metadata
