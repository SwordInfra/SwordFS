// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileHandle.hpp"

#include "vfs/InodeHandle.hpp"

namespace swordfs::vfs {

// ────────────────────────────────────────────────────────────────
// FileHandle
// ────────────────────────────────────────────────────────────────

FileHandle::FileHandle(std::shared_ptr<InodeHandle> handle) : handle_(std::move(handle)) {
}

utils::Status FileHandle::Open(metadata::InodeID ino, int flags, std::shared_ptr<FileHandle> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("file handle output is null");
  }

  auto inode_handle = InodeHandleManager::Instance().Get(ino, /*create_if_missing=*/true);
  if (!inode_handle) {
    return utils::Status::Internal("failed to get or create InodeHandle");
  }
  // Metadata fetch, permission check all live on the shared per-inode
  // InodeHandle.
  auto status = inode_handle->Open(flags);
  if (!status.ok()) {
    return status;
  }

  auto handle = std::make_shared<FileHandle>(inode_handle);
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

utils::Status FileHandle::Flush() {
  return handle_->Flush();
}

}  // namespace swordfs::vfs
