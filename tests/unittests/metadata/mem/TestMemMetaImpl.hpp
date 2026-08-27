// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <sys/stat.h>
#include <cstring>

#include "metadata/mem/MemMetaImpl.hpp"

namespace swordfs::metadata::test {

inline void AttrToStat(const SwordFsAttr &attr, struct stat *st) {
  std::memset(st, 0, sizeof(*st));
  st->st_ino = static_cast<ino_t>(attr.ino);
  st->st_mode = static_cast<mode_t>(attr.mode);
  st->st_nlink = static_cast<nlink_t>(attr.nlink);
  st->st_uid = static_cast<uid_t>(attr.uid);
  st->st_gid = static_cast<gid_t>(attr.gid);
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

inline SwordFsAttr StatToAttr(const struct stat &st) {
  SwordFsAttr attr;
  attr.dev = st.st_dev;
  attr.ino = st.st_ino;
  attr.mode = st.st_mode;
  attr.nlink = st.st_nlink;
  attr.uid = st.st_uid;
  attr.gid = st.st_gid;
  attr.rdev = st.st_rdev;
  attr.size = st.st_size;
  attr.blksize = st.st_blksize;
  attr.blocks = st.st_blocks;
  attr.atime = st.st_atime;
  attr.atime_nsec = st.st_atim.tv_nsec;
  attr.mtime = st.st_mtime;
  attr.mtime_nsec = st.st_mtim.tv_nsec;
  attr.ctime = st.st_ctime;
  attr.ctime_nsec = st.st_ctim.tv_nsec;
  return attr;
}

class TestMemMetaImpl : public MemMetaImpl {
 public:
  using MemMetaImpl::Create;
  using MemMetaImpl::Lookup;
  using MemMetaImpl::MkDir;

  Status SetAttr(InodeID ino, const struct stat *attr, SetAttrField fields,
                 SwordFsInode *out = nullptr) {
    return MemMetaImpl::SetAttr(ino, StatToAttr(*attr), fields, out);
  }

  Status Create(InodeID parent_ino, std::string_view name, mode_t mode,
                InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::Create(parent_ino, name, mode,
                                        (ino || attr) ? &out : nullptr);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) AttrToStat(out.attr, attr);
    }
    return status;
  }

  Status MkDir(InodeID parent_ino, std::string_view name, mode_t mode,
               InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::MkDir(parent_ino, name, mode,
                                       (ino || attr) ? &out : nullptr);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) AttrToStat(out.attr, attr);
    }
    return status;
  }

  Status Lookup(InodeID parent_ino, std::string_view name,
                InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::Lookup(parent_ino, name, &out);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) AttrToStat(out.attr, attr);
    }
    return status;
  }

  Status GetAttr(InodeID ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::GetInode(ino, attr ? &out : nullptr);
    if (status.ok() && attr) AttrToStat(out.attr, attr);
    return status;
  }
};

}  // namespace swordfs::metadata::test
