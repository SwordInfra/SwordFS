// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/InodeHandle.hpp"

#include <folly/container/F14Map.h>

#include <mutex>

#include "metadata/IMetaEngine.hpp"
#include "vfs/FileReadWriter.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

InodeHandle::InodeHandle(metadata::InodeID ino) : ino_(ino), rw_(std::make_shared<FileReadWriter>(ino)) {
}

utils::Status InodeHandle::Open(int flags) {
  // Take the descriptor reference before the metadata check: the reclaim
  // fence is claimed under the same lock, so a concurrent reclaim either
  // sees this reference (and refuses) or this caller sees the fence (and
  // fails) — an inode can never be frozen while a descriptor is being opened
  // on it.
  if (!AcquireRefUnlessReclaiming()) {
    return utils::Status::NotFound("inode is being reclaimed");
  }

  // Performs existing-inode validation and the atime update.
  auto meta = volume::VolumeImpl::Instance().meta_engine();
  uint64_t authoritative_size = 0;
  auto status = meta->Open(ino_, &authoritative_size);
  if (!status.ok()) {
    ReleaseRef();
    return status;
  }
  rw_->InitializeAuthoritativeSize(authoritative_size);

  if (flags & O_TRUNC) {
    status = rw_->Truncate(0);
    if (!status.ok()) {
      ReleaseRef();
      return status;
    }
  }
  return utils::Status::OK();
}

utils::Status InodeHandle::OpenCreated() {
  if (!AcquireRefUnlessReclaiming()) {
    return utils::Status::NotFound("inode is being reclaimed");
  }
  rw_->InitializeAuthoritativeSize(0);
  return utils::Status::OK();
}

utils::Status InodeHandle::Read(size_t size, off_t off, folly::IOBuf *out) {
  return rw_->Read(size, off, out);
}

utils::Status InodeHandle::Write(const folly::IOBuf &buf, off_t off) {
  return rw_->Write(buf, off);
}

utils::Status InodeHandle::GetAttr(metadata::SwordFsInode *out) const {
  return rw_->GetAttr(out);
}

LiveAttrGuard InodeHandle::LockLiveAttr() const {
  return LiveAttrGuard(rw_);
}

utils::Status InodeHandle::SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields,
                                   metadata::SwordFsInode *out) {
  return rw_->SetAttr(attr, fields, out);
}

utils::Status InodeHandle::Flush() {
  return rw_->Flush();
}

utils::Status InodeHandle::Close() {
  if (!PrepareClose()) {
    return utils::Status::OK();
  }

  auto status = rw_->Flush();
  // Keep the closing descriptor counted until the flush above completes. A
  // concurrent Open can therefore acquire a normal reference while a
  // concurrent unlink/reconcile sees this descriptor and defers reclaim.
  // Only after the flush do we drop our reference. Reclaim remains entirely
  // background-owned; a durable orphan candidate survives until a worker pass
  // observes this count at zero and claims the fence.
  ReleaseRef();
  return status;
}

uint64_t InodeHandle::open_count() const {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  return open_count_;
}

bool InodeHandle::AcquireRefUnlessReclaiming() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  if (reclaim_started_) {
    return false;
  }
  ++open_count_;
  return true;
}

bool InodeHandle::PrepareClose() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  CHECK_GT(open_count_, 0) << "InodeHandle::PrepareClose without a matching Open on inode " << ino_;
  if (open_count_ == 1) {
    // Keep the last descriptor counted until its Flush completes. This is the
    // key lifetime invariant: close-in-progress is still a live reference.
    return true;
  }
  --open_count_;
  return false;
}

void InodeHandle::ReleaseRef() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  // Every release is paired with a successful AcquireRefUnlessReclaiming(). An
  // underflow would wrap the count to a huge value and silently defer every
  // later reclaim of this inode forever, so catch the imbalance where it is.
  CHECK_GT(open_count_, 0) << "InodeHandle::ReleaseRef without a matching Open on inode " << ino_;
  --open_count_;
}

bool InodeHandle::TryStartReclaim() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  if (open_count_ != 0 || reclaim_started_) {
    return false;
  }
  reclaim_started_ = true;
  return true;
}

void InodeHandle::FinishReclaim() {
  std::lock_guard<utils::FiberMutex> lock(state_mutex_);
  CHECK(reclaim_started_) << "InodeHandle::FinishReclaim without a claimed fence on inode " << ino_;
  reclaim_started_ = false;
}

// ────────────────────────────────────────────────────────────────
// InodeHandleManager
// ────────────────────────────────────────────────────────────────

// Concrete F14FastMap type — hidden from the header to avoid pulling the
// heavy Folly template into every includer.
struct InodeHandleMap : folly::F14FastMap<metadata::InodeID, std::weak_ptr<InodeHandle>> {};

InodeHandleManager::InodeHandleManager() : inode_handles_(std::make_unique<InodeHandleMap>()) {
}

InodeHandleManager &InodeHandleManager::Instance() {
  static InodeHandleManager instance;
  return instance;
}

void InodeHandleManager::Initialize() {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  inode_handles_->clear();
}

std::shared_ptr<InodeHandle> InodeHandleManager::Get(metadata::InodeID ino, bool create_if_missing) {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = inode_handles_->find(ino);
  if (it != inode_handles_->end()) {
    if (auto handle = it->second.lock()) {
      return handle;
    }
    // The entry expired (all references dropped): drop it so the registry
    // does not grow without bound across a mount's lifetime. Reclaim
    // reconciliation visits one inode at a time, so this matters there.
    inode_handles_->erase(it);
  }

  if (!create_if_missing) {
    return nullptr;
  }

  auto handle = std::make_shared<InodeHandle>(ino);
  (*inode_handles_)[ino] = handle;
  return handle;
}

}  // namespace swordfs::vfs
