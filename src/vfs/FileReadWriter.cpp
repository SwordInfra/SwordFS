// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileReadWriter.hpp"

#include <fcntl.h>
#include <folly/fibers/Baton.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <shared_mutex>
#include <vector>

#include "chunk/ChunkFactory.hpp"
#include "metadata/IMetaEngine.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {

namespace {

// ────────────────────────────────────────────────────────────────
// MultiChunkReadWriter — dispatches concurrent chunk reads on
// folly fibers.  Call SubmitRead() for each chunk, then Collect()
// to block until all complete.
// ────────────────────────────────────────────────────────────────

class MultiChunkReadWriter {
 public:
  using Status = utils::Status;

  /// Submit a read from |c| at chunk-relative |off| for up to |len|
  /// bytes into |window|.  |window| should be a takeOwnership IOBuf
  /// pointing to the correct slice of the parent output buffer.
  void SubmitRead(std::shared_ptr<chunk::Chunk> c, size_t off, size_t len, std::unique_ptr<folly::IOBuf> window) {
    auto p = std::make_unique<Pending>();
    p->window = std::move(window);
    auto &fm = folly::fibers::FiberManager::getFiberManager();
    fm.addTask([c = std::move(c), off, len, raw = p.get()] {
      raw->status = c->Read(off, len, raw->window.get());
      raw->baton.post();
    });
    ops_.push_back(std::move(p));
  }

  /// Block until all submitted reads finish.  Returns the first
  /// non-OK status, or OK.
  Status Collect() {
    Drain();

    // All submitted fibers are complete, so it is safe to inspect status.
    // Chunk::Read owns the exact-byte-count contract for each operation.
    for (auto &p : ops_) {
      if (!p->status.ok()) {
        return p->status;
      }
    }
    return Status::OK();
  }

  /// Wait for every submitted read to finish without changing which later
  /// lookup error the caller returns. Each fiber lambda dereferences |raw|
  /// (a pointer into its Pending), so no path may destroy this object while
  /// an operation remains in flight.
  void Drain() {
    for (auto &p : ops_) {
      p->baton.wait();
    }
  }

 private:
  struct Pending {
    folly::fibers::Baton baton;
    std::unique_ptr<folly::IOBuf> window;
    Status status;
  };
  std::vector<std::unique_ptr<Pending>> ops_;
};

class MultiChunkFlusher {
 public:
  using Status = utils::Status;

  void Submit(std::shared_ptr<chunk::Chunk> chunk) {
    auto pending = std::make_unique<Pending>();
    pending->index = chunk->Index();
    auto &fm = folly::fibers::FiberManager::getFiberManager();
    fm.addTask([chunk = std::move(chunk), raw = pending.get()] {
      raw->status = chunk->Flush();
      raw->baton.post();
    });
    ops_.push_back(std::move(pending));
  }

  Status Collect(metadata::InodeID ino) {
    for (auto &pending : ops_) {
      pending->baton.wait();
    }
    Status first_error;
    for (auto &pending : ops_) {
      if (!pending->status.ok()) {
        SWORDFS_LOG_ERROR << "FileReadWriter::Flush chunk FAILED: ino=" << ino << " chunk=" << pending->index << " — "
                          << pending->status.message();
        if (first_error.ok()) {
          first_error = pending->status;
        }
      }
    }
    return first_error;
  }

 private:
  struct Pending {
    folly::fibers::Baton baton;
    metadata::ChunkIndex index = 0;
    Status status;
  };
  std::vector<std::unique_ptr<Pending>> ops_;
};

}  // namespace

// ────────────────────────────────────────────────────────────────
// FileChunkManager
// ────────────────────────────────────────────────────────────────

utils::Status FileChunkManager::Get(metadata::ChunkIndex idx, bool create_if_missing,
                                    std::shared_ptr<chunk::Chunk> *out) {
  if (out == nullptr) {
    return utils::Status::InvalidArgument("FileChunkManager::Get output is null");
  }
  out->reset();
  {
    std::lock_guard<utils::FiberMutex> lock(mutex_);
    auto it = chunks_.find(idx);
    if (it != chunks_.end()) {
      *out = it->second;
      return utils::Status::OK();
    }
  }

  if (factory_ == nullptr) {
    return utils::Status::Internal("FileChunkManager requires a chunk factory");
  }
  std::shared_ptr<chunk::Chunk> session;
  auto status = factory_->Open(ino_, idx, create_if_missing, &session);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = chunks_.find(idx);
  if (it != chunks_.end()) {
    *out = it->second;
  } else if (session != nullptr) {
    it = chunks_.try_emplace(idx, std::move(session)).first;
    *out = it->second;
  }
  return utils::Status::OK();
}

std::vector<std::shared_ptr<chunk::Chunk>> FileChunkManager::GetPendingWrites() {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  std::vector<std::shared_ptr<chunk::Chunk>> flushable;
  for (auto &[idx, chunk] : chunks_) {
    if (chunk->HasPendingWrites()) {
      flushable.push_back(chunk);
    }
  }
  return flushable;
}

void FileChunkManager::TruncateToSize(size_t size, size_t chunk_size) {
  CHECK(size <= metadata::kMaxSupportedFileSize) << "cached truncate exceeds supported file size: " << size;
  metadata::ChunkPosition boundary;
  const auto status = metadata::CalculateChunkPosition(static_cast<off_t>(size), chunk_size, &boundary);
  CHECK(status.ok()) << "invalid cached truncate boundary: size=" << size << " chunk_size=" << chunk_size;
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  const auto boundary_idx = boundary.index;
  const auto boundary_size = static_cast<size_t>(boundary.offset_in_chunk);
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    if (it->first > boundary_idx || (it->first == boundary_idx && boundary_size == 0)) {
      it = chunks_.erase(it);
    } else if (it->first == boundary_idx) {
      it->second->TruncateLocal(boundary_size);
      ++it;
    } else {
      ++it;
    }
  }
}

// ────────────────────────────────────────────────────────────────
// FileReadWriter
// ────────────────────────────────────────────────────────────────

FileReadWriter::FileReadWriter(InodeID ino, size_t max_parallel_flushes)
    : ino_(ino),
      chunk_size_(volume::VolumeImpl::Instance().chunk_size()),
      max_parallel_flushes_(std::max<size_t>(1, max_parallel_flushes)),
      meta_(volume::VolumeImpl::Instance().meta_engine()),
      chunks_(ino, volume::VolumeImpl::Instance().chunk_factory()) {
}

// ────────────────────────────────────────────────────────────────
// Write
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Write(const folly::IOBuf &buf, off_t off, int open_flags) {
  std::shared_lock<utils::FiberRWMutex> operation_lock(operation_mutex_);
  const auto inode_flags = inode_flags_.load(std::memory_order_acquire);
  if (metadata::HasInodeFlag(inode_flags, metadata::InodeFlag::kImmutable)) {
    return utils::Status::NotPermitted("immutable inode rejects write");
  }
  if (metadata::HasInodeFlag(inode_flags, metadata::InodeFlag::kAppendOnly) && (open_flags & O_APPEND) == 0) {
    return utils::Status::NotPermitted("append-only inode requires O_APPEND");
  }
  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: ino=" << ino_ << " size=" << buf.length() << " off=" << off;
  const size_t write_size = buf.length();
  uint64_t write_end = 0;
  auto status = metadata::CalculateFileRangeEnd(off, write_size, &write_end);
  if (!status.ok()) {
    return status;
  }
  size_t remaining = buf.length();
  uint64_t cursor = static_cast<uint64_t>(off);

  while (remaining > 0) {
    metadata::ChunkPosition position;
    status = metadata::CalculateChunkPosition(static_cast<off_t>(cursor), chunk_size_, &position);
    if (!status.ok()) {
      return status;
    }

    std::shared_ptr<chunk::Chunk> c;
    status = chunks_.Get(position.index, /*create_if_missing=*/true, &c);
    if (!status.ok()) {
      return status;
    }
    if (!c) {
      return utils::Status::Internal("FileReadWriter::Write: chunk lookup succeeded without a chunk");
    }

    const size_t room = chunk_size_ - static_cast<size_t>(position.offset_in_chunk);
    size_t n = std::min(remaining, room);
    auto slice = folly::IOBuf::takeOwnership(
        const_cast<uint8_t *>(buf.data()) + static_cast<size_t>(cursor - static_cast<uint64_t>(off)), n, n,
        +[](void *, void *) {}, nullptr, false);
    status = c->Write(static_cast<size_t>(position.offset_in_chunk), *slice);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileReadWriter::Write FAILED: ino=" << ino_ << " off=" << cursor << " chunk=" << c->Index()
                        << " — " << status.message();
      return status;
    }
    remaining -= n;
    cursor += n;
  }
  if (write_size != 0) {
    std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
    live_size_ = std::max(live_size_.value_or(0), write_end);
    ++size_state_epoch_;
    ++write_epoch_;
  }
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Read
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Read(size_t size, off_t off, folly::IOBuf *out) {
  std::shared_lock<utils::FiberRWMutex> operation_lock(operation_mutex_);
  if (off < 0) {
    return utils::Status::InvalidArgument("FileReadWriter::Read: negative offset");
  }
  if (size == 0) {
    return utils::Status::OK();
  }

  uint64_t visible_size = 0;
  auto status = GetVisibleSize(&visible_size);
  if (!status.ok()) {
    return status;
  }
  if (visible_size > metadata::kMaxSupportedFileSize) {
    return utils::Status::Malformed("FileReadWriter::Read: file size exceeds supported range");
  }
  const auto read_start = static_cast<uint64_t>(off);
  if (read_start >= visible_size) {
    return utils::Status::OK();
  }

  const uint64_t available = visible_size - read_start;
  const size_t read_size = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(size), available));
  MultiChunkReadWriter multi;
  size_t remaining = read_size;
  uint64_t cursor = read_start;
  auto *const write_start = out->writableData();

  while (remaining > 0) {
    // 1) Try the unified chunk map (dirty + flushed).
    metadata::ChunkPosition position;
    status = metadata::CalculateChunkPosition(static_cast<off_t>(cursor), chunk_size_, &position);
    if (!status.ok()) {
      multi.Drain();
      return status;
    }
    std::shared_ptr<chunk::Chunk> c;
    status = chunks_.Get(position.index, /*create_if_missing=*/false, &c);
    if (!status.ok()) {
      multi.Drain();
      return status;
    }

    if (c != nullptr) {
      const auto chunk_off = static_cast<size_t>(position.offset_in_chunk);
      const size_t window_cap = std::min(remaining, chunk_size_ - chunk_off);

      auto window = folly::IOBuf::takeOwnership(
          write_start + static_cast<size_t>(cursor - read_start), window_cap, static_cast<std::size_t>(0),
          +[](void *, void *) {}, nullptr, false);

      multi.SubmitRead(c, chunk_off, window_cap, std::move(window));
      remaining -= window_cap;
      cursor += window_cap;
      continue;
    }

    // 2) Hole — fill with zeros up to the next chunk boundary.
    size_t hole = std::min(remaining, chunk_size_ - static_cast<size_t>(position.offset_in_chunk));
    std::memset(write_start + static_cast<size_t>(cursor - read_start), 0, hole);
    remaining -= hole;
    cursor += hole;
  }

  status = multi.Collect();
  if (!status.ok()) {
    return status;
  }

  out->append(read_size - remaining);
  return Status::OK();
}

utils::Status FileReadWriter::GetAttr(metadata::SwordFsInode *out) const {
  std::shared_lock<utils::FiberRWMutex> operation_lock(operation_mutex_);
  uint64_t snapshot_epoch = 0;
  {
    std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
    snapshot_epoch = size_state_epoch_;
  }
  auto status = meta_->GetInode(ino_, out);
  if (!status.ok()) {
    return status;
  }
  {
    std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
    if (size_state_epoch_ == snapshot_epoch) {
      authoritative_size_ = out->attr.size;
    } else if (authoritative_size_.has_value()) {
      out->attr.size = std::max(out->attr.size, *authoritative_size_);
    }
    if (live_size_.has_value()) {
      out->attr.size = std::max(out->attr.size, *live_size_);
    }
  }
  return utils::Status::OK();
}

void FileReadWriter::InitializeCreatedState(uint64_t size) {
  // A fresh FileReadWriter already starts with kNone policy. Do not write
  // policy here: CREATE can race with a newer same-mount flag transition on
  // an already-published InodeHandle, and initialization must not roll it back.
  std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
  if (!authoritative_size_.has_value()) {
    authoritative_size_ = size;
    ++size_state_epoch_;
  }
}

uint64_t FileReadWriter::SnapshotInodeFlagEpoch() const {
  std::lock_guard<utils::FiberMutex> policy_lock(inode_policy_mutex_);
  return inode_flag_epoch_;
}

utils::Status FileReadWriter::ReconcileOpenState(uint64_t size, metadata::InodeFlag inode_flags,
                                                 uint64_t observed_flag_epoch, int open_flags) {
  std::lock_guard<utils::FiberMutex> policy_lock(inode_policy_mutex_);

  // A same-mount flag transition that completed while metadata Open was in
  // flight is newer than that Open snapshot. Preserve the locally committed
  // policy instead of restoring stale metadata state.
  if (inode_flag_epoch_ == observed_flag_epoch) {
    inode_flags_.store(inode_flags, std::memory_order_release);
  }

  const auto effective_inode_flags = inode_flags_.load(std::memory_order_acquire);
  const bool writable = (open_flags & O_ACCMODE) != O_RDONLY;
  if (metadata::HasInodeFlag(effective_inode_flags, metadata::InodeFlag::kImmutable) && writable) {
    return utils::Status::NotPermitted("immutable inode rejects writable open");
  }
  if (metadata::HasInodeFlag(effective_inode_flags, metadata::InodeFlag::kAppendOnly) && writable &&
      (((open_flags & O_APPEND) == 0) || ((open_flags & O_TRUNC) != 0))) {
    return utils::Status::NotPermitted("append-only inode requires O_APPEND and rejects O_TRUNC");
  }

  std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
  if (!authoritative_size_.has_value()) {
    authoritative_size_ = size;
    ++size_state_epoch_;
  }
  return utils::Status::OK();
}

utils::Status FileReadWriter::GetVisibleSize(uint64_t *size) {
  CHECK(size != nullptr);
  {
    std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
    if (authoritative_size_.has_value()) {
      *size = std::max(*authoritative_size_, live_size_.value_or(0));
      return utils::Status::OK();
    }
  }

  metadata::SwordFsInode inode;
  auto status = meta_->GetInode(ino_, &inode);
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
  if (!authoritative_size_.has_value()) {
    authoritative_size_ = inode.attr.size;
    ++size_state_epoch_;
  }
  *size = std::max(*authoritative_size_, live_size_.value_or(0));
  return utils::Status::OK();
}

LiveAttrGuard::LiveAttrGuard(std::shared_ptr<FileReadWriter> owner)
    : owner_(std::move(owner)), lock_(owner_->operation_mutex_) {
}

void LiveAttrGuard::Apply(metadata::SwordFsInode &inode) const {
  owner_->ApplyLiveSize(&inode);
}

void FileReadWriter::ApplyLiveSize(metadata::SwordFsInode *inode) const {
  std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
  if (inode != nullptr && live_size_.has_value()) {
    inode->attr.size = std::max(inode->attr.size, *live_size_);
  }
}

// ────────────────────────────────────────────────────────────────
// Flush
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Flush() {
  std::lock_guard<utils::FiberMutex> flush_lock(flush_mutex_);
  std::shared_lock<utils::FiberRWMutex> operation_lock(operation_mutex_);
  return FlushPendingWritesLocked();
}

utils::Status FileReadWriter::FlushPendingWritesLocked() {
  uint64_t barrier_epoch = 0;
  std::optional<uint64_t> barrier_live_size;
  {
    std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
    barrier_epoch = write_epoch_;
    barrier_live_size = live_size_;
  }
  utils::Status first_error;
  auto flushable = chunks_.GetPendingWrites();
  for (size_t begin = 0; begin < flushable.size(); begin += max_parallel_flushes_) {
    MultiChunkFlusher flusher;
    const size_t end = std::min(flushable.size(), begin + max_parallel_flushes_);
    for (size_t i = begin; i < end; ++i) {
      flusher.Submit(flushable[i]);
    }
    auto status = flusher.Collect(ino_);
    if (!status.ok() && first_error.ok()) {
      first_error = status;
    }
  }

  if (first_error.ok()) {
    std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
    if (barrier_live_size.has_value()) {
      authoritative_size_ = std::max(authoritative_size_.value_or(0), *barrier_live_size);
    }
    if (write_epoch_ == barrier_epoch) {
      live_size_.reset();
    }
    if (barrier_live_size.has_value()) {
      ++size_state_epoch_;
    }
  }

  return first_error;
}

utils::Status FileReadWriter::SetInodeFlags(metadata::InodeFlag inode_flags, metadata::SwordFsInode *out) {
  if (!metadata::HasOnlySupportedInodeFlags(inode_flags)) {
    return utils::Status::InvalidArgument("unsupported inode flags");
  }
  std::lock_guard<utils::FiberRWMutex> operation_lock(operation_mutex_);
  if (inode_flags != inode_flags_.load(std::memory_order_acquire) && inode_flags != metadata::InodeFlag::kNone) {
    auto status = FlushPendingWritesLocked();
    if (!status.ok()) {
      return status;
    }
  }

  auto status = meta_->SetInodeFlags(ino_, inode_flags, out);
  if (!status.ok()) {
    return status;
  }
  {
    std::lock_guard<utils::FiberMutex> policy_lock(inode_policy_mutex_);
    inode_flags_.store(inode_flags, std::memory_order_release);
    ++inode_flag_epoch_;
  }
  ApplyLiveSize(out);
  return utils::Status::OK();
}

utils::Status FileReadWriter::Truncate(size_t size) {
  std::lock_guard<utils::FiberRWMutex> operation_lock(operation_mutex_);
  if (size > metadata::kMaxSupportedFileSize) {
    return utils::Status::InvalidArgument("FileReadWriter::Truncate: size exceeds supported range");
  }
  auto status = meta_->Truncate(ino_, size);
  if (!status.ok()) {
    return status;
  }
  chunks_.TruncateToSize(size, chunk_size_);
  // Truncate persists the logical size synchronously; after success metadata
  // is authoritative and no transient size overlay remains necessary.
  {
    std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
    authoritative_size_ = size;
    live_size_.reset();
    ++size_state_epoch_;
  }
  return utils::Status::OK();
}

utils::Status FileReadWriter::SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields,
                                      metadata::SwordFsInode *out) {
  std::lock_guard<utils::FiberRWMutex> operation_lock(operation_mutex_);
  if (metadata::HasSetAttrField(fields, metadata::SetAttrField::kSize) && attr.size > metadata::kMaxSupportedFileSize) {
    return utils::Status::InvalidArgument("FileReadWriter::SetAttr: size exceeds supported range");
  }
  auto status = meta_->SetAttr(ino_, attr, fields, out);
  if (!status.ok()) {
    return status;
  }
  if (metadata::HasSetAttrField(fields, metadata::SetAttrField::kSize)) {
    chunks_.TruncateToSize(attr.size, chunk_size_);
    // A successful size setattr has already committed the new logical size.
    {
      std::lock_guard<utils::FiberMutex> size_lock(size_mutex_);
      authoritative_size_ = attr.size;
      live_size_.reset();
      ++size_state_epoch_;
    }
  }
  ApplyLiveSize(out);
  return utils::Status::OK();
}

}  // namespace swordfs::vfs
