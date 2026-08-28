// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

struct stat;

namespace swordfs::metadata {

/// Platform-independent inode attributes with fixed-width scalar fields.
struct SwordFsAttr {
  uint64_t dev = 0;
  uint64_t ino = 0;
  uint32_t mode = 0;
  uint64_t nlink = 0;
  uint64_t uid = 0;
  uint64_t gid = 0;
  uint64_t rdev = 0;
  uint64_t size = 0;
  uint64_t blksize = 0;
  uint64_t blocks = 0;

  int64_t atime = 0;
  int64_t atime_nsec = 0;
  int64_t mtime = 0;
  int64_t mtime_nsec = 0;
  int64_t ctime = 0;
  int64_t ctime_nsec = 0;

  SwordFsAttr() = default;
  SwordFsAttr(uint64_t ino, uint32_t mode);

  void KillSUID();

  void ToPosixStat(struct stat *st) const;
  static SwordFsAttr FromPosixStat(const struct stat &st);
};

/// Core per-inode metadata record.
struct SwordFsInode {
  InodeID ino = 0;
  SwordFsAttr attr{};
  InodeID parent_ino = 0;
  std::string symlink_target;

  SwordFsInode() = default;
  SwordFsInode(InodeID ino, SwordFsAttr attr, InodeID parent_ino, std::string symlink_target = std::string{});

  void Touch(SetAttrField fields);

  bool IsDir() const;
  bool IsRegular() const;
  bool IsSymlink() const;
  bool CheckAccess(uint64_t uid, uint64_t gid, uint32_t mask) const;
  bool CheckStickyDelete(uint64_t uid, const SwordFsInode &target) const;

  utils::Status SerializeTo(std::string *out) const;
  utils::Status ParseFrom(std::string_view data);
};

}  // namespace swordfs::metadata
