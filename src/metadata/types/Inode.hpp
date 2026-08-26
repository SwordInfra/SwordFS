// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sys/stat.h>
#include <sys/types.h>

#include <string>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

/// Core per-inode metadata record.
struct SwordFsInode {
  InodeID ino = 0;
  struct stat attr{};
  InodeID parent_ino = 0;
  std::string symlink_target;

  SwordFsInode() = default;
  SwordFsInode(InodeID ino, struct stat attr, InodeID parent_ino, std::string symlink_target = std::string{});

  void Touch(SetAttrField fields);

  bool IsDir() const;
  bool IsRegular() const;
  bool IsSymlink() const;
  bool CheckAccess(uid_t uid, gid_t gid, int mask) const;
  bool CheckStickyDelete(uid_t uid, const SwordFsInode &target) const;

  utils::Status SerializeTo(std::string *out) const;
  static utils::Status ParseFrom(std::string_view data, SwordFsInode *out);
};

}  // namespace swordfs::metadata
