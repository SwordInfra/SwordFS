// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "metadata/types/Inode.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

inline bool IsImmutable(const SwordFsAttr &attr) {
  return HasInodeFlag(attr.inode_flags, InodeFlag::kImmutable);
}

inline bool IsAppendOnly(const SwordFsAttr &attr) {
  return HasInodeFlag(attr.inode_flags, InodeFlag::kAppendOnly);
}

inline bool HasRestrictiveInodePolicy(const SwordFsAttr &attr) {
  return IsImmutable(attr) || IsAppendOnly(attr);
}

inline utils::Status CheckSetAttrPolicy(const SwordFsAttr &attr, SetAttrField fields) {
  if (IsImmutable(attr)) {
    return utils::Status::NotPermitted("immutable inode rejects setattr");
  }
  if (!IsAppendOnly(attr)) {
    return utils::Status::OK();
  }

  // Touch-now and the metadata consequences of an otherwise permitted append
  // write (ctime/killpriv) remain legal. Explicit metadata replacement and
  // explicit timestamp values do not.
  constexpr uint32_t kAlwaysForbidden =
      static_cast<uint32_t>(SetAttrField::kMode) | static_cast<uint32_t>(SetAttrField::kUid) |
      static_cast<uint32_t>(SetAttrField::kGid) | static_cast<uint32_t>(SetAttrField::kSize);
  const bool explicit_atime =
      HasSetAttrField(fields, SetAttrField::kAtime) && !HasSetAttrField(fields, SetAttrField::kAtimeNow);
  const bool explicit_mtime =
      HasSetAttrField(fields, SetAttrField::kMtime) && !HasSetAttrField(fields, SetAttrField::kMtimeNow);
  if ((static_cast<uint32_t>(fields) & kAlwaysForbidden) != 0 || explicit_atime || explicit_mtime) {
    return utils::Status::NotPermitted("append-only inode rejects setattr");
  }
  return utils::Status::OK();
}

inline utils::Status CheckContentMetadataMutationPolicy(const SwordFsAttr &attr, std::string_view operation) {
  if (HasRestrictiveInodePolicy(attr)) {
    return utils::Status::NotPermitted(std::string(operation) + " rejected by inode policy");
  }
  return utils::Status::OK();
}

inline utils::Status CheckDirectoryAdditionPolicy(const SwordFsAttr &parent) {
  if (IsImmutable(parent)) {
    return utils::Status::NotPermitted("immutable directory rejects entry addition");
  }
  return utils::Status::OK();
}

inline utils::Status CheckDirectoryRemovalPolicy(const SwordFsAttr &parent) {
  if (HasRestrictiveInodePolicy(parent)) {
    return utils::Status::NotPermitted("directory policy rejects entry removal");
  }
  return utils::Status::OK();
}

}  // namespace swordfs::metadata
