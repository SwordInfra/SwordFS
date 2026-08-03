// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileReadWriter.hpp"

#include <folly/fibers/Baton.h>
#include <folly/io/IOBuf.h>

#include <algorithm>
#include <vector>

#include "chunk/Chunk.hpp"
#include "chunk/ChunkManager.hpp"
#include <folly/logging/xlog.h>
#include "utils/Logging.hpp"

namespace swordfs::vfs {

namespace {

// ────────────────────────────────────────────────────────────────
// MultiChunkReadWriter — dispatches concurrent chunk reads on
// folly fibers.  Call SubmitRead() for each chunk, then Collect()
// to block until all complete.
// ────────────────────────────────────────────────────────────────

class MultiChunkReadWriter {
 public:
  using Status = FileReadWriter::Status;

  /// Submit a read from |c| at chunk-relative |off| for up to |len|
  /// bytes into |window|.  |window| should be a takeOwnership IOBuf
  /// pointing to the correct slice of the parent output buffer.
  void SubmitRead(chunk::Chunk *c, off_t off, size_t len,
                  std::unique_ptr<folly::IOBuf> window) {
    auto p = std::make_unique<Pending>();
    p->window = std::move(window);
    folly::fibers::addTask([c, off, len, raw = p.get()] {
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

FileReadWriter::FileReadWriter(uint64_t fh, InodeID ino, size_t chunk_size)
    : fh_(fh), ino_(ino), chunk_size_(chunk_size) {}

// ────────────────────────────────────────────────────────────────
// Write — split across chunk boundaries.
//
// Unlike Read, Write does NOT use concurrent fibers because:
//   1. GetOrCreateChunk() mutates the chunk deque state and always
//      returns dq.back() — concurrent calls would race on creation
//      order and chunk indexing.
//   2. Chunk::Write() is currently an in-memory memcpy; the real
//      async work (seal + S3 upload) happens later in Flush().
// ────────────────────────────────────────────────────────────────

FileReadWriter::Status FileReadWriter::Write(const folly::IOBuf &buf, off_t off) {
  auto &mgr = chunk::ChunkManager::Instance();
  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: ino=" << ino_
                    << " size=" << buf.length() << " off=" << off;
  size_t remaining = buf.length();
  off_t cur_off = off;

  while (remaining > 0) {
    auto &c = mgr.GetOrCreateChunk(fh_, ino_, cur_off);
    size_t room = chunk_size_ - (cur_off % chunk_size_);
    size_t n = std::min(remaining, room);
    auto slice = folly::IOBuf::takeOwnership(
        const_cast<uint8_t*>(buf.data()) + (cur_off - off),
        n, (std::size_t)n,
        +[](void*, void*) {}, nullptr, true);
    auto status = c.Write(cur_off, *slice);
    if (!status.ok()) return status;
    remaining -= n;
    cur_off += static_cast<off_t>(n);
  }
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Read — query ChunkManager singleton for in-flight chunks
// ────────────────────────────────────────────────────────────────

FileReadWriter::Status FileReadWriter::Read(size_t size, off_t off, folly::IOBuf *out) {
  auto &mgr = chunk::ChunkManager::Instance();
  auto *const write_start = out->writableData();

  MultiChunkReadWriter multi;
  size_t remaining = size;
  off_t cur_off = off;
  size_t total = 0;

  while (remaining > 0) {
    auto *c = mgr.FindChunk(ino_, cur_off);
    if (!c) {
      size_t hole = std::min(remaining, chunk_size_);
      std::memset(write_start + static_cast<size_t>(cur_off - off), 0, hole);
      total += hole;
      remaining -= hole;
      cur_off += static_cast<off_t>(hole);
      continue;
    }

    off_t chunk_off = cur_off - c->StartOffset();
    // Cap the window at the chunk's actual data extent — if the
    // chunk is only partially filled, the remainder becomes a hole.
    size_t window_cap = std::min(remaining,
                                 static_cast<size_t>(c->EndOffset() - cur_off));

    auto window = folly::IOBuf::takeOwnership(
        write_start + static_cast<size_t>(cur_off - off),
        window_cap,
        (std::size_t)0,
        +[](void *, void *) {},
        nullptr,
        true);

    multi.SubmitRead(c, chunk_off, window_cap, std::move(window));
    remaining -= window_cap;
    cur_off += static_cast<off_t>(window_cap);
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

FileReadWriter::Status FileReadWriter::Flush() {
  return chunk::ChunkManager::Instance().Flush(fh_);
}

}  // namespace swordfs::vfs
