// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileReadWriter.hpp"

#include <folly/fibers/Baton.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <shared_mutex>
#include <vector>

#include "chunk/Chunk.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "utils/Logging.hpp"
#include "vfs/Reclaimer.hpp"
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
  void SubmitRead(std::shared_ptr<chunk::Chunk> c, off_t off, size_t len, std::unique_ptr<folly::IOBuf> window) {
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
    pending->index = chunk->index();
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

  auto chunk = std::make_shared<chunk::Chunk>(ino_, idx);
  auto status = chunk->Initialize();
  if (!status.ok()) {
    return status;
  }

  std::lock_guard<utils::FiberMutex> lock(mutex_);
  auto it = chunks_.find(idx);
  if (it != chunks_.end()) {
    *out = it->second;
  } else if (chunk->IsClean() || create_if_missing) {
    it = chunks_.try_emplace(idx, std::move(chunk)).first;
    *out = it->second;
  }
  return utils::Status::OK();
}

std::vector<std::shared_ptr<chunk::Chunk>> FileChunkManager::GetFlushable() {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  std::vector<std::shared_ptr<chunk::Chunk>> flushable;
  for (auto &[idx, chunk] : chunks_) {
    if (chunk->Flushable()) {
      flushable.push_back(chunk);
    }
  }
  return flushable;
}

void FileChunkManager::TruncateToSize(size_t size, size_t chunk_size) {
  std::lock_guard<utils::FiberMutex> lock(mutex_);
  const auto boundary_idx = static_cast<metadata::ChunkIndex>(size / chunk_size);
  const size_t boundary_size = size % chunk_size;
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    if (it->first > boundary_idx || (it->first == boundary_idx && boundary_size == 0)) {
      it = chunks_.erase(it);
    } else if (it->first == boundary_idx) {
      it->second->Truncate(boundary_size);
      ++it;
    } else {
      ++it;
    }
  }
}

// ────────────────────────────────────────────────────────────────
// FileReadWriter
// ────────────────────────────────────────────────────────────────

FileReadWriter::FileReadWriter(InodeID ino)
    : ino_(ino),
      chunk_size_(volume::VolumeImpl::Instance().chunk_size()),
      meta_(volume::VolumeImpl::Instance().meta_engine()),
      chunks_(ino) {
}

// ────────────────────────────────────────────────────────────────
// Write
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Write(const folly::IOBuf &buf, off_t off) {
  std::shared_lock<utils::FiberRWMutex> operation_lock(operation_mutex_);
  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: ino=" << ino_ << " size=" << buf.length() << " off=" << off;
  const size_t write_size = buf.length();
  size_t remaining = buf.length();
  off_t cur_off = off;

  while (remaining > 0) {
    metadata::ChunkIndex idx = static_cast<metadata::ChunkIndex>(cur_off / static_cast<off_t>(chunk_size_));

    std::shared_ptr<chunk::Chunk> c;
    auto status = chunks_.Get(idx, /*create_if_missing=*/true, &c);
    if (!status.ok()) {
      return status;
    }
    if (!c) {
      return utils::Status::Internal("FileReadWriter::Write: chunk lookup succeeded without a chunk");
    }

    size_t room = chunk_size_ - (cur_off % chunk_size_);
    size_t n = std::min(remaining, room);
    auto slice = folly::IOBuf::takeOwnership(
        const_cast<uint8_t *>(buf.data()) + (cur_off - off), n, n, +[](void *, void *) {}, nullptr, false);
    status = c->Write(cur_off, *slice);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileReadWriter::Write FAILED: ino=" << ino_ << " off=" << cur_off << " chunk=" << c->index()
                        << " — " << status.message();
      return status;
    }
    remaining -= n;
    cur_off += static_cast<off_t>(n);
  }
  if (write_size != 0) {
    const auto write_end = static_cast<uint64_t>(cur_off);
    std::lock_guard<utils::FiberMutex> live_size_lock(live_size_mutex_);
    live_size_ = std::max(live_size_.value_or(0), write_end);
    ++write_epoch_;
  }
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Read
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Read(size_t size, off_t off, folly::IOBuf *out) {
  std::shared_lock<utils::FiberRWMutex> operation_lock(operation_mutex_);
  MultiChunkReadWriter multi;
  size_t remaining = size;
  off_t cur_off = off;
  auto *const write_start = out->writableData();

  while (remaining > 0) {
    // 1) Try the unified chunk map (dirty + flushed).
    metadata::ChunkIndex idx = static_cast<metadata::ChunkIndex>(cur_off / static_cast<off_t>(chunk_size_));
    std::shared_ptr<chunk::Chunk> c;
    auto status = chunks_.Get(idx, /*create_if_missing=*/false, &c);
    if (!status.ok()) {
      multi.Drain();
      return status;
    }

    // cur_off may fall within the chunk's index range (e.g. a 64 MiB
    // chunk that only has 500 bytes of data — offsets [500, 64 MiB)
    // are holes that still map to the same idx).  We must guard with
    // DataEnd() because `static_cast<size_t>(DataEnd - cur_off)`
    // would overflow to a huge value when cur_off ≥ DataEnd, leading
    // to a bogus window_cap and an infinite loop.
    bool has_data = (c != nullptr) && (cur_off < c->DataEnd());

    if (has_data) {
      off_t chunk_off = cur_off - c->StartOffset();
      size_t window_cap = std::min(remaining, static_cast<size_t>(c->DataEnd() - cur_off));
      CHECK(window_cap > 0) << "window_cap=0: cur_off=" << cur_off << " DataEnd=" << c->DataEnd()
                            << " remaining=" << remaining;

      auto window = folly::IOBuf::takeOwnership(
          write_start + static_cast<size_t>(cur_off - off), window_cap, static_cast<std::size_t>(0),
          +[](void *, void *) {}, nullptr, false);

      multi.SubmitRead(c, chunk_off, window_cap, std::move(window));
      remaining -= window_cap;
      cur_off += static_cast<off_t>(window_cap);
      continue;
    }

    // 2) Hole — fill with zeros up to the next chunk boundary.
    size_t hole = std::min(remaining, chunk_size_ - static_cast<size_t>(cur_off % chunk_size_));
    std::memset(write_start + static_cast<size_t>(cur_off - off), 0, hole);
    remaining -= hole;
    cur_off += static_cast<off_t>(hole);
  }

  auto status = multi.Collect();
  if (!status.ok()) {
    return status;
  }

  out->append(size - remaining);
  return Status::OK();
}

utils::Status FileReadWriter::GetAttr(metadata::SwordFsInode *out) const {
  std::shared_lock<utils::FiberRWMutex> operation_lock(operation_mutex_);
  auto status = meta_->GetInode(ino_, out);
  if (!status.ok()) {
    return status;
  }
  ApplyLiveSize(out);
  return utils::Status::OK();
}

LiveAttrGuard::LiveAttrGuard(std::shared_ptr<FileReadWriter> owner)
    : owner_(std::move(owner)), lock_(owner_->operation_mutex_) {
}

void LiveAttrGuard::Apply(metadata::SwordFsInode &inode) const {
  owner_->ApplyLiveSize(&inode);
}

void FileReadWriter::ApplyLiveSize(metadata::SwordFsInode *inode) const {
  std::lock_guard<utils::FiberMutex> live_size_lock(live_size_mutex_);
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
  uint64_t barrier_epoch = 0;
  {
    std::lock_guard<utils::FiberMutex> live_size_lock(live_size_mutex_);
    barrier_epoch = write_epoch_;
  }
  utils::Status first_error;
  auto flushable = chunks_.GetFlushable();
  const auto &config = config::ConfigCenter::Instance();
  const size_t max_parallel = static_cast<size_t>(std::max(1, config.storage_thread_count()));
  for (size_t begin = 0; begin < flushable.size(); begin += max_parallel) {
    MultiChunkFlusher flusher;
    const size_t end = std::min(flushable.size(), begin + max_parallel);
    for (size_t i = begin; i < end; ++i) {
      flusher.Submit(flushable[i]);
    }
    auto status = flusher.Collect(ino_);
    if (!status.ok() && first_error.ok()) {
      first_error = status;
    }
  }

  if (!flushable.empty()) {
    // Rewrite success or a definite rejection may have registered best-effort
    // cleanup. Physical deletion is centralized in the Reclaimer, whose
    // authoritative metadata check is the delete-safety boundary.
    Reclaimer::Instance().Wake();
  }

  if (first_error.ok()) {
    std::lock_guard<utils::FiberMutex> live_size_lock(live_size_mutex_);
    if (write_epoch_ == barrier_epoch) {
      live_size_.reset();
    }
  }

  return first_error;
}

utils::Status FileReadWriter::Truncate(size_t size) {
  std::lock_guard<utils::FiberRWMutex> operation_lock(operation_mutex_);
  auto status = meta_->Truncate(ino_, size);
  if (!status.ok()) {
    return status;
  }
  chunks_.TruncateToSize(size, chunk_size_);
  // Truncate persists the logical size synchronously; after success metadata
  // is authoritative and no transient size overlay remains necessary.
  {
    std::lock_guard<utils::FiberMutex> live_size_lock(live_size_mutex_);
    live_size_.reset();
  }
  Reclaimer::Instance().Wake();
  return utils::Status::OK();
}

utils::Status FileReadWriter::SetAttr(const metadata::SwordFsAttr &attr, metadata::SetAttrField fields,
                                      metadata::SwordFsInode *out) {
  std::lock_guard<utils::FiberRWMutex> operation_lock(operation_mutex_);
  auto status = meta_->SetAttr(ino_, attr, fields, out);
  if (!status.ok()) {
    return status;
  }
  if (metadata::HasSetAttrField(fields, metadata::SetAttrField::kSize)) {
    chunks_.TruncateToSize(attr.size, chunk_size_);
    // A successful size setattr has already committed the new logical size.
    {
      std::lock_guard<utils::FiberMutex> live_size_lock(live_size_mutex_);
      live_size_.reset();
    }
    Reclaimer::Instance().Wake();
  }
  ApplyLiveSize(out);
  return utils::Status::OK();
}

}  // namespace swordfs::vfs
