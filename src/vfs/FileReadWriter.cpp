// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileReadWriter.hpp"

#include <algorithm>

#include "chunk/ChunkManager.hpp"
#include "utils/Logging.hpp"

namespace swordfs::vfs {

FileReadWriter::FileReadWriter(uint64_t fh, InodeID ino, size_t chunk_size)
    : fh_(fh), ino_(ino), chunk_size_(chunk_size) {}

// ────────────────────────────────────────────────────────────────
// Write — split across chunk boundaries
// ────────────────────────────────────────────────────────────────

FileReadWriter::Status FileReadWriter::Write(const char *data, size_t size, off_t off) {
  auto &mgr = chunk::ChunkManager::Instance();
  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: ino=" << ino_
                    << " size=" << size << " off=" << off;
  const char *pos = data;
  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    auto &c = mgr.GetOrCreateChunk(fh_, ino_, cur_off);
    size_t room = chunk_size_ - (cur_off % chunk_size_);
    size_t n = std::min(remaining, room);
    auto status = c.Write(pos, n, cur_off);
    if (!status.ok()) return status;
    pos += n;
    remaining -= n;
    cur_off += static_cast<off_t>(n);
  }

  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: done ino=" << ino_
                    << " total=" << size;
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Read — query ChunkManager singleton for in-flight chunks
// ────────────────────────────────────────────────────────────────

FileReadWriter::Status FileReadWriter::Read(size_t size, off_t off, folly::IOBuf *out) {
  auto &mgr = chunk::ChunkManager::Instance();
  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    auto *c = mgr.FindChunk(ino_, cur_off);
    if (!c) break;  // never written — EOF

    off_t chunk_off = cur_off - c->StartOffset();
    size_t before = out->length();
    auto status = c->Read(chunk_off, remaining, out);
    if (!status.ok()) return status;
    size_t n = out->length() - before;
    if (n == 0) break;
    remaining -= n;
    cur_off += static_cast<off_t>(n);
  }

  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Flush
// ────────────────────────────────────────────────────────────────

FileReadWriter::Status FileReadWriter::Flush() {
  return chunk::ChunkManager::Instance().Flush(fh_);
}

}  // namespace swordfs::vfs
