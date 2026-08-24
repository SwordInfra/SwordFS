// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include "metadata/mem/MemMetaImpl.hpp"

namespace swordfs::metadata::test {

class TestMemMetaImpl : public MemMetaImpl {
 public:
  using MemMetaImpl::Create;
  using MemMetaImpl::Lookup;
  using MemMetaImpl::MkDir;

  Status Create(InodeID parent_ino, std::string_view name, mode_t mode,
                InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::Create(parent_ino, name, mode,
                                        (ino || attr) ? &out : nullptr);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) *attr = out.attr;
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
      if (attr) *attr = out.attr;
    }
    return status;
  }

  Status Lookup(InodeID parent_ino, std::string_view name,
                InodeID *ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::Lookup(parent_ino, name, &out);
    if (status.ok()) {
      if (ino) *ino = out.ino;
      if (attr) *attr = out.attr;
    }
    return status;
  }

  Status GetAttr(InodeID ino, struct stat *attr) {
    SwordFsInode out;
    Status status = MemMetaImpl::GetInode(ino, attr ? &out : nullptr);
    if (status.ok() && attr) *attr = out.attr;
    return status;
  }
};

}  // namespace swordfs::metadata::test
