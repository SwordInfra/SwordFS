// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/DirHandle.hpp"

#include <glog/logging.h>

#include "vfs/Handle.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

utils::Status DirHandle::Open(metadata::InodeID ino, std::shared_ptr<DirHandle> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("directory handle output is null");
  }

  metadata::DirIteratorPtr iterator;
  auto status = volume::VolumeImpl::Instance().meta_engine()->OpenDir(ino, &iterator);
  if (!status.ok()) {
    return status;
  }

  auto handle = std::make_shared<DirHandle>(std::move(iterator));
  HandleManager::Instance().Register(handle);
  *out = std::move(handle);
  return utils::Status::OK();
}

utils::Status DirHandle::Release() {
  HandleManager::Instance().Unregister(fh());
  return utils::Status::OK();
}

utils::Status DirHandle::ReadDir(off_t off, size_t size, const DirEntryEncoder &encoder, std::string *out) {
  CHECK(out != nullptr);
  out->clear();
  if (size == 0) {
    return utils::Status::OK();
  }

  auto status = iterator_->Seek(static_cast<uint64_t>(off));
  if (!status.ok()) {
    return status;
  }

  while (out->size() < size) {
    metadata::SwordFsEntry entry;
    uint64_t next_off = 0;
    status = iterator_->Peek(&entry, &next_off);
    if (status.IsEndOfDirectory()) {
      break;
    }
    if (!status.ok()) {
      return status;
    }

    const size_t available = size - out->size();
    const size_t required = encoder.CalSpace(entry, static_cast<off_t>(next_off));
    if (required > available) {
      if (out->empty()) {
        return utils::Status::NoMemory("readdir entry does not fit in buffer");
      }
      break;
    }

    encoder.Encode(entry, static_cast<off_t>(next_off), required, out);
    iterator_->Advance();
  }
  return utils::Status::OK();
}

}  // namespace swordfs::vfs
