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
      access_bits = attr.st_mode & S_IRWXO;
    }

    if ((mask & R_OK) && !(access_bits & R_OK)) return false;
    if ((mask & W_OK) && !(access_bits & W_OK)) return false;
    if ((mask & X_OK) && !(access_bits & X_OK)) return false;
    return true;
  }
};

}  // namespace swordfs::metadata
