// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <memory>
#include <string>

#include "metadata/IMetaEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/Handle.hpp"

namespace swordfs::vfs {

class DirEntryEncoder {
 public:
  virtual ~DirEntryEncoder() = default;

  virtual size_t CalSpace(const metadata::SwordFsEntry &entry, off_t next_off) const = 0;
  virtual void Encode(const metadata::SwordFsEntry &entry, off_t next_off, size_t required, std::string *out) const = 0;
};

class DirHandle : public Handle {
 public:
  explicit DirHandle(metadata::DirIteratorPtr iterator) : iterator_(std::move(iterator)) {
  }

  static utils::Status Open(metadata::InodeID ino, std::shared_ptr<DirHandle> *out);
  utils::Status Release();
  utils::Status ReadDir(off_t off, size_t size, const DirEntryEncoder &encoder, std::string *out);

 private:
  metadata::DirIteratorPtr iterator_;
};

}  // namespace swordfs::vfs
