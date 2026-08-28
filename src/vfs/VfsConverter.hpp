// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sys/stat.h>

#include "metadata/types/Common.hpp"
#include "metadata/types/Inode.hpp"

namespace swordfs::vfs {

void ToPosixStat(const metadata::SwordFsAttr& attr, struct stat* st);

metadata::SwordFsAttr FromPosixStat(const struct stat& st);

metadata::SetAttrField FromFuseSetAttrFields(int fuse_to_set);

metadata::RenameFlag FromFuseRenameFlags(unsigned int fuse_flags);

}  // namespace swordfs::vfs
