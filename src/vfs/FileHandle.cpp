// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileHandle.hpp"

#include <fcntl.h>

#include "vfs/InodeHandle.hpp"

namespace swordfs::vfs {

// ────────────────────────────────────────────────────────────────
// FileHandle
// ────────────────────────────────────────────────────────────────

FileHandle::FileHandle(std::shared_ptr<InodeHandle> handle, int flags) : handle_(std::move(handle)), flags_(flags) {
}

utils::Status FileHandle::Open(metadata::InodeID ino, int flags, std::shared_ptr<FileHandle> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("file handle output is null");
  }

  auto inode_handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/true);
  // Existing-inode validation and open-time metadata side effects live on
  // the shared per-inode InodeHandle. Caller authorization has already been
  // completed by Linux VFS/FUSE.
  auto status = inode_handle->Open(flags);
  if (!status.ok()) {
    return status;
  }

  auto handle = std::make_shared<FileHandle>(inode_handle, flags);
  HandleManager::Instance().Register(handle);
  *out = std::move(handle);
  return utils::Status::OK();
}

utils::Status FileHandle::Create(metadata::InodeID ino, int flags, std::shared_ptr<FileHandle> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("file handle output is null");
  }

  auto inode_handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/true);
  auto status = inode_handle->OpenCreated();
  if (!status.ok()) {
    return status;
  }

  auto handle = std::make_shared<FileHandle>(inode_handle, flags);
  HandleManager::Instance().Register(handle);
  *out = std::move(handle);
  return utils::Status::OK();
}

utils::Status FileHandle::Release() {
  auto status = handle_->Close();
  HandleManager::Instance().Unregister(fh());
  return status;
}

utils::Status FileHandle::Read(size_t size, off_t off, folly::IOBuf *out) {
  return handle_->Read(size, off, out);
}

utils::Status FileHandle::Write(const folly::IOBuf &buf, off_t off) {
  return handle_->Write(buf, off);
}

utils::Status FileHandle::SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields,
                                  metadata::SwordFsInode *out) {
  if (metadata::HasSetAttrField(fields, metadata::SetAttrField::kSize) && !writable()) {
    return utils::Status::InvalidArgument("file handle is not writable");
  }
  return handle_->SetAttr(attr, fields, out);
}

bool FileHandle::writable() const {
  return (flags_ & O_ACCMODE) != O_RDONLY;
}

utils::Status FileHandle::Flush() {
  return handle_->Flush();
}

}  // namespace swordfs::vfs
