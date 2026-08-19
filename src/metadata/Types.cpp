// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/Types.hpp"

#include <ctime>

namespace swordfs::metadata {

SwordFsInode::SwordFsInode(InodeID ino, struct stat attr,
                           InodeID parent_ino, uint64_t nlookup,
                           std::string symlink_target)
    : ino(ino),
      attr(attr),
      parent_ino(parent_ino),
      nlookup(nlookup),
      symlink_target(std::move(symlink_target)) {}

void SwordFsInode::Touch(SetAttrField fields) {
  time_t now = ::time(nullptr);
  if (HasSetAttrField(fields, SetAttrField::kAtime)) {
    attr.st_atime = now;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtime)) {
    attr.st_mtime = now;
  }
  if (HasSetAttrField(fields, SetAttrField::kCtime)) {
    attr.st_ctime = now;
  }
}

bool SwordFsInode::IsDir() const { return S_ISDIR(attr.st_mode); }

bool SwordFsInode::CheckAccess(uid_t uid, gid_t gid, int mask) const {
  if (uid == 0) {
    return true;
  }

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

}  // namespace swordfs::metadata