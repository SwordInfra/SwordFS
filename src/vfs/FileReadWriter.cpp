// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "vfs/FileReadWriter.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include "metadata/Meta.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

namespace swordfs::vfs {

FileReadWriter::FileReadWriter(metadata::IMetaEngine *meta,
                               storage::IDataEngine *data,
                               InodeID ino)
    : meta_(meta), data_(data), ino_(ino) {}

// ────────────────────────────────────────────────────────────────
// Chunk management
// ────────────────────────────────────────────────────────────────

chunk::Chunk &FileReadWriter::GetOrCreateChunk(off_t off) {
  size_t max_chunk = data_->Limits().max_chunk_size;
  uint32_t idx = static_cast<uint32_t>(off / static_cast<off_t>(max_chunk));

  while (static_cast<uint32_t>(chunks_.size()) <= idx) {
    uint32_t next = chunks_.empty() ? 0 : chunks_.back().index() + 1;
    chunks_.emplace_back(ino_, next, max_chunk);
  }
  return chunks_.back();
}

// ────────────────────────────────────────────────────────────────
// Write — split across chunk boundaries
// ────────────────────────────────────────────────────────────────

Status FileReadWriter::Write(const char *data, size_t size, off_t off) {
  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: ino=" << ino_
                    << " size=" << size << " off=" << off;
  size_t max_chunk = data_->Limits().max_chunk_size;
  const char *pos = data;
  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    auto &c = GetOrCreateChunk(cur_off);
    size_t room = max_chunk - (cur_off % max_chunk);
    size_t n = std::min(remaining, room);
    SWORDFS_LOG_DEBUG << "FileReadWriter::Write: chunk=" << c.index()
                      << " cur_off=" << cur_off << " n=" << n;
    Status status = c.Write(pos, n, cur_off);
    if (!status.ok()) return status;
    pos += n;
    remaining -= n;
    cur_off += static_cast<off_t>(n);
  }

  SWORDFS_LOG_DEBUG << "FileReadWriter::Write: done ino=" << ino_
                    << " total=" << size << " chunks=" << chunks_.size();
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Read — merge in-flight chunk buffers with storage
// ────────────────────────────────────────────────────────────────

Status FileReadWriter::Read(size_t size, off_t off, folly::IOBuf *out) {
  size_t chunk_sz = data_->Limits().max_chunk_size;
  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    // 1. Search own chunks.
    chunk::Chunk *found = nullptr;
    for (auto &c : chunks_) {
      if (cur_off >= c.StartOffset() && cur_off < c.EndOffset()) {
        found = &c;
        break;
      }
    }

    if (found) {
      off_t local_off = cur_off - found->StartOffset();
      size_t before = out->length();
      Status status = found->Read(local_off, remaining, out);
      if (!status.ok()) return status;
      size_t n = out->length() - before;
      remaining -= n;
      cur_off += static_cast<off_t>(n);
      if (n > 0) continue;
    }

    // 2. Serve from storage.
    {
      off_t chunk_idx = cur_off / static_cast<off_t>(chunk_sz);
      size_t chunk_off_val = static_cast<size_t>(
          cur_off % static_cast<off_t>(chunk_sz));
      std::string key =
          std::to_string(ino_) + "/" + std::to_string(chunk_idx);

      size_t chunk_actual = 0;
      if (!data_->Head(key, &chunk_actual) ||
          chunk_off_val >= chunk_actual) {
        break;  // EOF
      }

      size_t to_read = std::min(remaining, chunk_actual - chunk_off_val);
      std::string chunk_data;
      Status status =
          data_->Get(key, &chunk_data, chunk_off_val, to_read);
      if (!status.ok()) return status;

      std::memcpy(out->writableTail(), chunk_data.data(), chunk_data.size());
      out->append(chunk_data.size());
      remaining -= chunk_data.size();
      cur_off += static_cast<off_t>(chunk_data.size());

      if (chunk_data.size() < to_read) break;
    }
  }

  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Flush
// ────────────────────────────────────────────────────────────────

Status FileReadWriter::Flush() {
  SWORDFS_LOG_DEBUG << "FileReadWriter::Flush: ino=" << ino_
                    << " chunks=" << chunks_.size();
  off_t file_end = 0;
  for (auto &c : chunks_) {
    if (c.EndOffset() > file_end) file_end = c.EndOffset();
    if (c.IsWriting() && !c.empty()) c.Seal();
  }

  for (auto &c : chunks_) {
    if (!c.IsSealed()) continue;
    std::string_view data = c.FlushData();
    if (data.empty()) continue;

    std::string key(c.ChunkKey());
    std::string payload(data);
    SWORDFS_LOG_DEBUG << "FileReadWriter::Flush: uploading ino=" << c.ino()
                      << " chunk=" << c.index()
                      << " key=" << key << " size=" << payload.size();
    auto status = data_->Put(key, payload);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileReadWriter::Flush: upload FAILED ino="
                        << c.ino() << " chunk=" << c.index()
                        << " key=" << key << " — " << status.message();
      return status;
    }
    SWORDFS_LOG_INFO << "Flush uploaded: ino=" << c.ino()
                     << " chunk=" << c.index()
                     << " size=" << data.size();
  }

  if (file_end > 0) {
    struct stat attr;
    if (meta_->GetAttr(ino_, &attr).ok() && file_end > attr.st_size) {
      struct stat new_attr = {};
      new_attr.st_size = file_end;
      meta_->SetAttr(ino_, &new_attr, FUSE_SET_ATTR_SIZE, nullptr);
    }
  }

  chunks_.erase(std::remove_if(chunks_.begin(), chunks_.end(),
                               [](const chunk::Chunk &c) {
                                 return c.IsSealed();
                               }),
                chunks_.end());

  SWORDFS_LOG_DEBUG << "FileReadWriter::Flush: done ino=" << ino_;
  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// FindChunk — for cross-fh reads
// ────────────────────────────────────────────────────────────────

chunk::Chunk *FileReadWriter::FindChunk(InodeID ino, off_t off) {
  if (ino != ino_) return nullptr;
  for (auto &c : chunks_) {
    if (off >= c.StartOffset() && off < c.EndOffset()) return &c;
  }
  return nullptr;
}

}  // namespace swordfs::vfs
