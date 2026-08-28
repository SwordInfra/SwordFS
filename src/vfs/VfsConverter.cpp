// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/VfsConverter.hpp"

#include <cstring>

namespace swordfs::vfs {

void ToPosixStat(const metadata::SwordFsAttr& attr, struct stat* st) {
  if (st == nullptr) return;
  std::memset(st, 0, sizeof(*st));
  st->st_dev = static_cast<dev_t>(attr.dev);
  st->st_ino = static_cast<ino_t>(attr.ino);
  st->st_mode = static_cast<mode_t>(attr.mode);
  st->st_nlink = static_cast<nlink_t>(attr.nlink);
  st->st_uid = static_cast<uid_t>(attr.uid);
  st->st_gid = static_cast<gid_t>(attr.gid);
  st->st_rdev = static_cast<dev_t>(attr.rdev);
  st->st_size = static_cast<off_t>(attr.size);
  st->st_blksize = static_cast<blksize_t>(attr.blksize);
  st->st_blocks = static_cast<blkcnt_t>(attr.blocks);
  st->st_atime = static_cast<time_t>(attr.atime);
  st->st_atim.tv_nsec = static_cast<long>(attr.atime_nsec);
  st->st_mtime = static_cast<time_t>(attr.mtime);
  st->st_mtim.tv_nsec = static_cast<long>(attr.mtime_nsec);
  st->st_ctime = static_cast<time_t>(attr.ctime);
  st->st_ctim.tv_nsec = static_cast<long>(attr.ctime_nsec);
}

metadata::SwordFsAttr FromPosixStat(const struct stat& st) {
  metadata::SwordFsAttr attr;
  attr.dev = static_cast<uint64_t>(st.st_dev);
  attr.ino = static_cast<uint64_t>(st.st_ino);
  attr.mode = static_cast<uint32_t>(st.st_mode);
  attr.nlink = static_cast<uint64_t>(st.st_nlink);
  attr.uid = static_cast<uint64_t>(st.st_uid);
  attr.gid = static_cast<uint64_t>(st.st_gid);
  attr.rdev = static_cast<uint64_t>(st.st_rdev);
  attr.size = static_cast<uint64_t>(st.st_size);
  attr.blksize = static_cast<uint64_t>(st.st_blksize);
  attr.blocks = static_cast<uint64_t>(st.st_blocks);
  attr.atime = static_cast<int64_t>(st.st_atime);
  attr.atime_nsec = static_cast<int64_t>(st.st_atim.tv_nsec);
  attr.mtime = static_cast<int64_t>(st.st_mtime);
  attr.mtime_nsec = static_cast<int64_t>(st.st_mtim.tv_nsec);
  attr.ctime = static_cast<int64_t>(st.st_ctime);
  attr.ctime_nsec = static_cast<int64_t>(st.st_ctim.tv_nsec);
  return attr;
}

metadata::SetAttrField FromFuseSetAttrFields(int fuse_to_set) {
  return static_cast<metadata::SetAttrField>(static_cast<uint32_t>(fuse_to_set));
}

metadata::RenameFlag FromFuseRenameFlags(unsigned int fuse_flags) {
  return static_cast<metadata::RenameFlag>(fuse_flags);
}

}  // namespace swordfs::vfs
