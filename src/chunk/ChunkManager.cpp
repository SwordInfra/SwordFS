// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/ChunkManager.hpp"

#include "metadata/Meta.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

namespace swordfs::chunk {

ChunkManager::ChunkManager(metadata::IMetaEngine* meta,
                           storage::IDataEngine* data)
    : meta_(meta), data_(data) {}

// ────────────────────────────────────────────────────────────────
// Write path
// ────────────────────────────────────────────────────────────────

Status ChunkManager::Write(uint64_t fh, InodeID ino, const char* data,
                           size_t size, off_t off) {
  if (!data_) {
    return Status::Internal("no data engine configured");
  }

  // Get or create write buffer for this file handle.
  auto& wb = write_bufs_[fh];
  if (!wb.IsInit()) {
    wb.Init(ino, data_->Limits().max_chunk_size);
  }

  wb.Append(data, size, off);

  SWORDFS_LOG_DEBUG << "Write buffered: ino=" << ino << " offset=" << off
                    << " size=" << size
                    << " buf_total=" << wb.size();

  // Flush full chunks until the buffer is below the threshold.
  while (wb.ShouldFlush()) {
    Status st = Flush(fh, /*force=*/false);
    if (!st.ok()) return st;
  }

  return Status::OK();
}

Status ChunkManager::Flush(uint64_t fh, bool force) {
  auto it = write_bufs_.find(fh);
  if (it == write_bufs_.end()) {
    SWORDFS_LOG_DEBUG << "ChunkManager::Flush: fh=" << fh << " — no buffer";
    return Status::OK();
  }
  WriteBuf& wb = it->second;

  if (wb.empty()) return Status::OK();

  // When not forced, flush exactly one chunk and keep the remainder.
  // When forced (Flush/Release/Fsync), flush everything.
  if (!force && !wb.ShouldFlush()) {
    SWORDFS_LOG_DEBUG << "ChunkManager::Flush: fh=" << fh
                      << " buf=" << wb.size()
                      << " — below threshold (max=" << wb.max_chunk_size()
                      << ")";
    return Status::OK();
  }

  std::string_view flush_data = wb.FlushData(force);
  size_t flush_size = flush_data.size();

  std::string key = std::to_string(wb.ino()) + "/" +
                    std::to_string(wb.next_chunk());
  Status status = data_->Put(key, flush_data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "ChunkManager::Flush FAILED: ino=" << wb.ino()
                      << " chunk=" << wb.next_chunk()
                      << " size=" << flush_size
                      << " — " << status.message();
    return status;
  }

  SWORDFS_LOG_INFO << "ChunkManager::Flush: ino=" << wb.ino()
                   << " chunk=" << wb.next_chunk()
                   << " size=" << flush_size;

  // Update file size from actual write offsets.
  struct stat attr;
  if (meta_->GetAttr(wb.ino(), &attr).ok() &&
      wb.max_write_end() > attr.st_size) {
    struct stat new_attr = {};
    new_attr.st_size = wb.max_write_end();
    meta_->SetAttr(wb.ino(), &new_attr, FUSE_SET_ATTR_SIZE, nullptr);
  }

  wb.CommitFlush(flush_size);
  return Status::OK();
}

void ChunkManager::RemoveBuf(uint64_t fh) {
  write_bufs_.erase(fh);
}

// ────────────────────────────────────────────────────────────────
// Read path
// ────────────────────────────────────────────────────────────────

Status ChunkManager::Read(InodeID ino, size_t size, off_t off,
                          std::string* out) {
  if (!data_) {
    return Status::Internal("no data engine configured");
  }

  size_t chunk_sz = data_->Limits().max_chunk_size;
  out->clear();
  out->reserve(size);

  WriteBuf* wb = FindWriteBuf(ino);

  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    // ── Serve from write buffer if the current offset falls within it ──
    if (wb && cur_off >= wb->BufStart() && cur_off < wb->BufEnd()) {
      size_t n = wb->CopyOut(cur_off, remaining, out);
      remaining -= n;
      cur_off += static_cast<off_t>(n);
      continue;
    }

    // ── Serve from storage ──
    off_t chunk_idx = cur_off / static_cast<off_t>(chunk_sz);
    size_t chunk_off_val = static_cast<size_t>(
        cur_off % static_cast<off_t>(chunk_sz));
    std::string key =
        std::to_string(ino) + "/" + std::to_string(chunk_idx);

    size_t chunk_actual = 0;
    if (!data_->Head(key, &chunk_actual) ||
        chunk_off_val >= chunk_actual) {
      break;  // EOF — no more chunks (and nothing in buffer)
    }

    size_t to_read = std::min(remaining, chunk_actual - chunk_off_val);
    std::string chunk_data;
    Status status =
        data_->Get(key, &chunk_data, chunk_off_val, to_read);
    if (!status.ok()) {
      return status;
    }

    out->append(chunk_data);
    remaining -= chunk_data.size();
    cur_off += chunk_data.size();

    // If the engine returned fewer bytes than requested we've hit EOF.
    if (chunk_data.size() < to_read) break;
  }

  return Status::OK();
}

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

WriteBuf* ChunkManager::FindWriteBuf(InodeID ino) {
  for (auto& [fh, wb] : write_bufs_) {
    if (wb.ino() == ino && !wb.empty()) {
      return &wb;
    }
  }
  return nullptr;
}

}  // namespace swordfs::chunk
