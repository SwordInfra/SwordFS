// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/Utils.hpp"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cctype>
#include <utility>

#include "dirent.h"
#include "metadata/InodePolicy.hpp"
#include "metadata/PosixAcl.hpp"
#include "metadata/types/Common.hpp"

namespace swordfs::metadata {

utils::Status PrepareSetAttrMutation(const SwordFsInode &current, const SwordFsAttr &requested, SetAttrField fields,
                                     SwordFsInode *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("SetAttr mutation output is null");
  }
  auto status = CheckSetAttrPolicy(current.attr, fields);
  if (!status.ok()) {
    return status;
  }

  SwordFsInode next = current;
  auto &attr = next.attr;
  const bool size_changed = HasSetAttrField(fields, SetAttrField::kSize) && requested.size != attr.size;
  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    attr.mode = (attr.mode & S_IFMT) | (requested.mode & 07777);
  }
  if (HasSetAttrField(fields, SetAttrField::kUid)) {
    attr.uid = requested.uid;
  }
  if (HasSetAttrField(fields, SetAttrField::kGid)) {
    attr.gid = requested.gid;
  }
  if (HasSetAttrField(fields, SetAttrField::kSize)) {
    attr.size = requested.size;
  }
  if (HasSetAttrField(fields, SetAttrField::kAtime)) {
    attr.atime = requested.atime;
    attr.atime_nsec = requested.atime_nsec;
  }
  if (HasSetAttrField(fields, SetAttrField::kMtime)) {
    attr.mtime = requested.mtime;
    attr.mtime_nsec = requested.mtime_nsec;
  }
  if (HasSetAttrField(fields, SetAttrField::kAtimeNow)) {
    next.Touch(SetAttrField::kAtime);
  }
  if (HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
    next.Touch(SetAttrField::kMtime);
  }
  if (HasSetAttrField(fields, SetAttrField::kCtime)) {
    attr.ctime = requested.ctime;
    attr.ctime_nsec = requested.ctime_nsec;
  }
  if (HasSetAttrField(fields, SetAttrField::kKillSuidGid)) {
    attr.ClearSetidForKillPriv();
  }
  if (size_changed && !HasSetAttrField(fields, SetAttrField::kMtime) &&
      !HasSetAttrField(fields, SetAttrField::kMtimeNow)) {
    next.Touch(SetAttrField::kMtime);
  }
  if (!HasSetAttrField(fields, SetAttrField::kCtime)) {
    next.Touch(SetAttrField::kCtime);
  }
  if (HasSetAttrField(fields, SetAttrField::kMode)) {
    status = SyncPosixAccessAclForMode(&next, attr.mode);
    if (!status.ok()) {
      return status;
    }
  }
  *out = std::move(next);
  return utils::Status::OK();
}

CreateInheritance ResolveCreateInheritance(uint64_t caller_gid, const SwordFsAttr &parent, uint32_t child_mode) {
  CreateInheritance result{.gid = caller_gid, .mode = child_mode};
  if ((parent.mode & S_ISGID) != 0) {
    result.gid = parent.gid;
    if (S_ISDIR(child_mode)) {
      result.mode |= S_ISGID;
    }
  }
  return result;
}

// Convert st_mode to dirent type (DT_DIR, DT_REG, etc.)
uint32_t ModeToDt(uint32_t mode) {
  if (S_ISDIR(mode)) {
    return DT_DIR;
  }
  if (S_ISREG(mode)) {
    return DT_REG;
  }
  if (S_ISLNK(mode)) {
    return DT_LNK;
  }
  if (S_ISBLK(mode)) {
    return DT_BLK;
  }
  if (S_ISCHR(mode)) {
    return DT_CHR;
  }
  if (S_ISFIFO(mode)) {
    return DT_FIFO;
  }
  if (S_ISSOCK(mode)) {
    return DT_SOCK;
  }
  return DT_UNKNOWN;
}

utils::Status ValidateNameComponent(std::string_view name) {
  if (name.size() > kMaxNameLength) {
    return utils::Status::NameTooLong("name exceeds maximum length");
  }
  return utils::Status::OK();
}

utils::Status ValidateMknodMode(uint32_t mode) {
  switch (mode & S_IFMT) {
    case S_IFREG:
    case S_IFIFO:
    case S_IFCHR:
    case S_IFBLK:
    case S_IFSOCK:
      return utils::Status::OK();
    default:
      return utils::Status::InvalidArgument("unsupported mknod file type");
  }
}

uint64_t NormalizeMknodRdev(uint32_t mode, uint64_t rdev) {
  return S_ISCHR(mode) || S_ISBLK(mode) ? rdev : 0;
}

utils::Status ParseUrlScheme(std::string_view url, std::string *scheme) {
  if (scheme == nullptr) {
    return utils::Status::InvalidArgument("URL scheme output is null");
  }

  const auto pos = url.find("://");
  if (pos == std::string_view::npos || pos == 0) {
    return utils::Status::InvalidArgument("URL must have a scheme:// prefix: " + std::string(url));
  }

  scheme->assign(url.substr(0, pos));
  for (char &c : *scheme) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
