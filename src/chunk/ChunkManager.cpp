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
// Helpers
// ────────────────────────────────────────────────────────────────

std::deque<Chunk>& ChunkManager::GetChunks(uint64_t fh) {
  auto it = chunks_.find(fh);
  if (it == chunks_.end()) {
    it = chunks_.emplace(fh, std::deque<Chunk>{}).first;
  }
  return it->second;
}

Status ChunkManager::UploadChunk(Chunk& c) {
  std::string_view data = c.FlushData();
  if (data.empty()) return Status::OK();

  Status status = data_->Put(c.ChunkKey(), data);
  if (!status.ok()) {
    SWORDFS_LOG_ERROR << "ChunkManager::UploadChunk FAILED: ino=" << c.ino()
                      << " chunk=" << c.index()
                      << " size=" << data.size()
                      << " — " << status.message();
    return status;
  }

  SWORDFS_LOG_INFO << "ChunkManager::UploadChunk: ino=" << c.ino()
                   << " chunk=" << c.index()
                   << " size=" << data.size();
  return Status::OK();
}

Chunk* ChunkManager::FindChunk(InodeID ino, off_t off) {
  // Search all chunk deques for a chunk covering |off|.
  for (auto& [fh, dq] : chunks_) {
    if (dq.empty()) continue;
    if (dq.front().ino() != ino) continue;
    for (auto& c : dq) {
      if (off >= c.StartOffset() && off < c.EndOffset()) {
        return &c;
      }
    }
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────
// Write path
// ────────────────────────────────────────────────────────────────

Status ChunkManager::Write(uint64_t fh, InodeID ino, const char* data,
                           size_t size, off_t off) {
  if (!data_) {
    return Status::Internal("no data engine configured");
  }

  auto& dq = GetChunks(fh);
  size_t max_chunk = data_->Limits().max_chunk_size;
  const char* pos = data;
  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    // Create a new chunk if the deque is empty or the last chunk
    // has no room to extend (size() >= max_chunk).  The chunk is
    // NOT sealed here — that only happens on Flush/Release.
    if (dq.empty() || dq.back().size() >= max_chunk) {
      uint32_t idx = dq.empty() ? 0 : dq.back().index() + 1;
      dq.emplace_back(idx, ino, max_chunk);
    }

    Chunk& cur = dq.back();

    size_t room = max_chunk - cur.size();
    size_t n = std::min(remaining, room);
    Status st = cur.Write(pos, n, cur_off);
    if (!st.ok()) return st;

    SWORDFS_LOG_DEBUG << "Write buffered: ino=" << ino
                      << " offset=" << cur_off
                      << " size=" << n
                      << " chunk=" << cur.index()
                      << " buf_total=" << cur.size();

    pos += n;
    remaining -= n;
    cur_off += static_cast<off_t>(n);
  }

  return Status::OK();
}

Status ChunkManager::Flush(uint64_t fh) {
  auto it = chunks_.find(fh);
  if (it == chunks_.end()) return Status::OK();

  auto& dq = it->second;

  // Compute the highest file offset before removing anything.
  off_t file_end = 0;
  for (auto& c : dq) {
    if (c.EndOffset() > file_end) file_end = c.EndOffset();
  }

  // Seal all writing chunks.
  for (auto& c : dq) {
    if (c.IsWriting() && !c.empty()) c.Seal();
  }

  // Upload all sealed chunks.
  for (auto& c : dq) {
    if (c.IsSealed()) {
      Status st = UploadChunk(c);
      if (!st.ok()) return st;
    }
  }

  // Update file size.
  if (file_end > 0) {
    struct stat attr;
    if (meta_->GetAttr(dq.front().ino(), &attr).ok() &&
        file_end > attr.st_size) {
      struct stat new_attr = {};
      new_attr.st_size = file_end;
      meta_->SetAttr(dq.front().ino(), &new_attr, FUSE_SET_ATTR_SIZE, nullptr);
    }
  }

  // Remove uploaded chunks — reads will now go through storage.
  dq.erase(std::remove_if(dq.begin(), dq.end(),
                          [](const Chunk& c) { return c.IsSealed(); }),
           dq.end());

  return Status::OK();
}

Status ChunkManager::Release(uint64_t fh) {
  Status st = Flush(fh);
  chunks_.erase(fh);
  return st;
}

// ────────────────────────────────────────────────────────────────
// Read path
// ────────────────────────────────────────────────────────────────

Status ChunkManager::Read(InodeID ino, size_t size, off_t off,
                          folly::IOBuf* out) {
  if (!data_) {
    return Status::Internal("no data engine configured");
  }

  size_t chunk_sz = data_->Limits().max_chunk_size;
  out->reserve(0, size);  // pre-allocate tail room

  size_t remaining = size;
  off_t cur_off = off;

  while (remaining > 0) {
    // ── Try to serve from an in-flight (not yet uploaded) chunk ──
    Chunk* c = FindChunk(ino, cur_off);
    if (c && c->StartOffset() != static_cast<off_t>(-1)) {
      off_t local_off = cur_off - c->StartOffset();
      size_t before = out->length();
      Status st = c->CopyOut(local_off, remaining, out);
      if (!st.ok()) return st;
      size_t n = out->length() - before;
      remaining -= n;
      cur_off += static_cast<off_t>(n);
      if (n > 0) continue;  // try next offset in same chunk
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
      break;  // EOF
    }

    size_t to_read = std::min(remaining, chunk_actual - chunk_off_val);
    std::string chunk_data;
    Status status =
        data_->Get(key, &chunk_data, chunk_off_val, to_read);
    if (!status.ok()) {
      return status;
    }

    std::memcpy(out->writableTail(), chunk_data.data(), chunk_data.size());
    out->append(chunk_data.size());
    remaining -= chunk_data.size();
    cur_off += chunk_data.size();

    if (chunk_data.size() < to_read) break;
  }

  return Status::OK();
}

}  // namespace swordfs::chunk
