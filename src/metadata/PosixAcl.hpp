// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdint>
#include <string_view>

#include "metadata/types/Common.hpp"
#include "metadata/types/Inode.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {

inline constexpr std::string_view kPosixAclAccessXAttr = "system.posix_acl_access";
inline constexpr std::string_view kPosixAclDefaultXAttr = "system.posix_acl_default";

bool IsPosixAclXAttrName(std::string_view name);

/// Apply one Linux POSIX ACL xattr mutation to an inode. Access ACL mode
/// projection, minimal-ACL canonicalization, default-ACL placement and ctime
/// update are one inode-local semantic transition.
utils::Status SetPosixAclXAttr(SwordFsInode *inode, std::string_view name, std::string_view value, XAttrSetMode mode);

/// Remove one POSIX ACL xattr while keeping the already-synchronized mode as
/// the permission authority.
utils::Status RemovePosixAclXAttr(SwordFsInode *inode, std::string_view name);

/// Keep a stored extended access ACL synchronized with a chmod-style mode
/// change. The caller owns ctime and persistence so the entire setattr remains
/// one backend transaction.
utils::Status SyncPosixAccessAclForMode(SwordFsInode *inode, uint32_t mode);

/// Resolve create-time umask/default-ACL inheritance against the authoritative
/// parent inode. Symlinks are intentionally left unchanged.
utils::Status ApplyPosixAclCreateInheritance(const SwordFsInode &parent, uint32_t umask, SwordFsInode *child);

}  // namespace swordfs::metadata
