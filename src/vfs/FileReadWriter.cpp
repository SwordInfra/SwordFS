// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileReadWriter.hpp"

#include <folly/fibers/Baton.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <algorithm>
#include <vector>

#include "chunk/Chunk.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"
#include "volume/VolumeImpl.hpp"

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

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
  void SubmitRead(chunk::Chunk *c, off_t off, size_t len,
                  std::unique_ptr<folly::IOBuf> window) {
    auto p = std::make_unique<Pending>();
    p->window = std::move(window);
    auto &fm = folly::fibers::FiberManager::getFiberManager();
    fm.addTask([c, off, len, raw = p.get()] {
      raw->status = c->Read(off, len, raw->window.get());
      raw->bytes = raw->window->length();
      raw->baton.post();
    });
    ops_.push_back(std::move(p));
  }

  /// Block until all submitted reads finish.  Returns the first
  /// non-OK status, or OK.
  Status Collect() {
    for (auto &p : ops_) {
      p->baton.wait();
      if (!p->status.ok()) return p->status;
      total_ += p->bytes;
    }
    return Status::OK();
  }

  size_t TotalBytes() const { return total_; }

 private:
  struct Pending {
    folly::fibers::Baton baton;
    std::unique_ptr<folly::IOBuf> window;
    Status status;
    size_t bytes = 0;
  };
  std::vector<std::unique_ptr<Pending>> ops_;
  size_t total_ = 0;
};

}  // namespace

// ────────────────────────────────────────────────────────────────
// DirtyChunkHandler
// ────────────────────────────────────────────────────────────────

chunk::Chunk *DirtyChunkHandler::Get(metadata::ChunkIndex idx, off_t off,
                                     bool create_if_missing) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = chunks_.find(idx);
  if (it != chunks_.end() && off < it->second.EndOffset()) {
    return &it->second;
  }
  if (!create_if_missing) return nullptr;
  it = chunks_.try_emplace(idx, ino_, idx).first;
  return &it->second;
}

chunk::Chunk *DirtyChunkHandler::GetNextFlushable() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &[idx, c] : chunks_) {
    if (c.Flushable()) {
      c.Seal();
      return &c;
    }
  }
  return nullptr;
}

void DirtyChunkHandler::Remove(metadata::ChunkIndex idx) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = chunks_.find(idx);
  CHECK(it != chunks_.end()) << "Remove: chunk " << idx << " not found";
  CHECK(it->second.IsFlushed())
      << "Remove: chunk " << idx << " is not flushed";
  chunks_.erase(it);
}

// ────────────────────────────────────────────────────────────────
// FileReadWriter
// ────────────────────────────────────────────────────────────────

FileReadWriter::FileReadWriter(InodeID ino)
    : ino_(ino),
      chunk_size_(volume::VolumeImpl::Instance().chunk_size()),
      meta_(volume::VolumeImpl::Instance().meta_engine()),
      data_(volume::VolumeImpl::Instance().data_engine()),
      dirty_(ino) {}

// ────────────────────────────────────────────────────────────────
// Write
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Write(const folly::IOBuf &buf, off_t off) {
  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: ino=" << ino_
                    << " size=" << buf.length() << " off=" << off;
  size_t remaining = buf.length();
  off_t cur_off = off;

  while (remaining > 0) {
    metadata::ChunkIndex idx = static_cast<metadata::ChunkIndex>(
        cur_off / static_cast<off_t>(chunk_size_));

    auto &c = *dirty_.Get(idx, cur_off, /*create_if_missing=*/true);

    size_t room = chunk_size_ - (cur_off % chunk_size_);
    size_t n = std::min(remaining, room);
    auto slice = folly::IOBuf::takeOwnership(
        const_cast<uint8_t *>(buf.data()) + (cur_off - off),
        n, static_cast<std::size_t>(n),
        +[](void *, void *) {}, nullptr, true);
    auto status = c.Write(cur_off, *slice);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileReadWriter::Write FAILED: ino=" << ino_
                        << " off=" << cur_off
                        << " chunk=" << c.index() << " — "
                        << status.message();
      return status;
    }
    remaining -= n;
    cur_off += static_cast<off_t>(n);
  }
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Read
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Read(size_t size, off_t off, folly::IOBuf *out) {
  auto *const write_start = out->writableData();

  MultiChunkReadWriter multi;
  size_t remaining = size;
  off_t cur_off = off;
  size_t total = 0;

  while (remaining > 0) {
    // 1) Check dirty chunks (in-memory write buffer).
    metadata::ChunkIndex idx = static_cast<metadata::ChunkIndex>(
        cur_off / static_cast<off_t>(chunk_size_));
    auto *c = dirty_.Get(idx, cur_off, /*create_if_missing=*/false);
    if (c) {
      off_t chunk_off = cur_off - c->StartOffset();
      size_t window_cap =
          std::min(remaining,
                   static_cast<size_t>(c->EndOffset() - cur_off));

      auto window = folly::IOBuf::takeOwnership(
          write_start + static_cast<size_t>(cur_off - off),
          window_cap, static_cast<std::size_t>(0),
          +[](void *, void *) {}, nullptr, true);

      multi.SubmitRead(c, chunk_off, window_cap, std::move(window));
      remaining -= window_cap;
      cur_off += static_cast<off_t>(window_cap);
      continue;
    }

    // 2) Try the metadata engine (flushed chunks).
    metadata::ChunkMeta cm;
    auto status = meta_->FindChunk(ino_, cur_off, chunk_size_, &cm);
    if (status.ok()) {
      off_t chunk_end =
          static_cast<off_t>(cm.start_offset + cm.size);
      size_t window_cap =
          std::min(remaining,
                   static_cast<size_t>(chunk_end - cur_off));

      auto window = folly::IOBuf::takeOwnership(
          write_start + static_cast<size_t>(cur_off - off),
          window_cap, static_cast<std::size_t>(0),
          +[](void *, void *) {}, nullptr, true);
      status = data_->Get(cm.key,
                          static_cast<size_t>(cur_off - cm.start_offset),
                          window_cap, window.get());
      if (status.ok()) {
        total += window->length();
        remaining -= window->length();
        cur_off += static_cast<off_t>(window->length());
        continue;
      }
      // Chunk registered but not found in storage — fall through to hole.
    }

    // 3) Hole — fill with zeros up to the next chunk boundary.
    size_t hole =
        std::min(remaining,
                 chunk_size_ - static_cast<size_t>(cur_off % chunk_size_));
    std::memset(write_start + static_cast<size_t>(cur_off - off), 0, hole);
    total += hole;
    remaining -= hole;
    cur_off += static_cast<off_t>(hole);
  }

  auto status = multi.Collect();
  if (!status.ok()) return status;
  total += multi.TotalBytes();

  out->append(total);
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Flush
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Flush() {
  off_t file_end = 0;
  while (auto *c = dirty_.GetNextFlushable()) {
    auto idx = c->index();
    auto status = c->Flush();
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileReadWriter::Flush chunk FAILED: ino=" << ino_
                        << " chunk=" << idx
                        << " — " << status.message();
      continue;  // keep the sealed chunk, try the next one
    }
    meta_->AddChunk(ino_, c->BuildMeta());
    if (c->EndOffset() > file_end) file_end = c->EndOffset();
    dirty_.Remove(idx);
  }

  // Update file size if the file grew.
  if (file_end > 0) {
    struct stat attr;
    if (meta_->GetAttr(ino_, &attr).ok() &&
        file_end > attr.st_size) {
      struct stat new_attr = {};
      new_attr.st_size = file_end;
      meta_->SetAttr(ino_, &new_attr, FUSE_SET_ATTR_SIZE, nullptr);
    }
  }

  return Status::OK();
}

}  // namespace swordfs::vfs
