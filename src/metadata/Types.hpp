// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Core POD types for the metadata subsystem.
//
// This header does NOT pull in Folly (F14Map). If you need F14FastMap,
// include <folly/container/F14Map.h> directly.

#pragma once

#include <sys/stat.h>

#include <cstdint>
#include <ctime>
#include <string>

namespace swordfs::metadata {

// forward declaration — most users only hold pointers
struct SwordFsInode;

using InodeID = uint64_t;
using ChunkIndex = uint32_t;  // 0-based chunk number within a file

struct SwordFsEntry {
  std::string name;
  uint32_t type;  // DT_DIR, DT_REG, DT_LNK, etc.
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
  std::string symlink_target;  // non-empty only for S_IFLNK

  void Touch(uint8_t fields) {
    time_t now = ::time(nullptr);
    if (fields & kAtime) attr.st_atime = now;
    if (fields & kMtime) attr.st_mtime = now;
    if (fields & kCtime) attr.st_ctime = now;
  }

  bool IsDir() const { return S_ISDIR(attr.st_mode); }

  // POSIX access check. Returns true if uid/gid has the requested permissions
  // on this inode. Root (uid == 0) always has full access.
  bool CheckAccess(uid_t uid, gid_t gid, int mask) const {
    if (uid == 0) return true;

    unsigned int access_bits;
    if (uid == attr.st_uid) {
      access_bits = (attr.st_mode & S_IRWXU) >> 6;
    } else if (gid == attr.st_gid) {
      access_bits = (attr.st_mode & S_IRWXG) >> 3;
    } else {
      access_bits = (attr.st_mode & S_IRWXO);
    }
    return (access_bits & static_cast<unsigned int>(mask)) ==
           static_cast<unsigned int>(mask);
  }
};

/// Metadata for one flushed chunk.
struct ChunkMeta {
  off_t start_offset;  // file offset where this chunk begins
  std::string key;     // storage key (e.g. "42/0")
  size_t size;         // data size in bytes
};

}  // namespace swordfs::metadata
