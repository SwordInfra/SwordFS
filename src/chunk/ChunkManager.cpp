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

Chunk& ChunkManager::GetOrCreateChunk(uint64_t fh, InodeID ino,
                                           off_t off) {
  auto& dq = GetChunks(fh);
  size_t max_chunk = data_->Limits().max_chunk_size;

  // Find or create the chunk that covers |off|.
  uint32_t idx = static_cast<uint32_t>(off / static_cast<off_t>(max_chunk));

  // Create missing intermediate chunks (only the last one is writable).
  while (static_cast<uint32_t>(dq.size()) <= idx) {
    uint32_t next = dq.empty() ? 0 : dq.back().index() + 1;
    dq.emplace_back(ino, next, max_chunk);
  }

  return dq.back();
}

Status ChunkManager::Flush(uint64_t fh) {
  auto it = chunks_.find(fh);
  if (it == chunks_.end()) return Status::OK();

  auto& dq = it->second;

  off_t file_end = 0;
  for (auto& c : dq) {
    if (c.EndOffset() > file_end) file_end = c.EndOffset();
    if (c.IsWriting() && !c.empty()) c.Seal();
  }

  // Upload sealed chunks.
  for (auto& c : dq) {
    if (!c.IsSealed()) continue;
    std::string_view data = c.FlushData();
    if (data.empty()) continue;

    Status status = data_->Put(c.ChunkKey(), data);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "Flush FAILED: ino=" << c.ino()
                        << " chunk=" << c.index()
                        << " size=" << data.size()
                        << " — " << status.message();
      return status;
    }
    SWORDFS_LOG_INFO << "Flush uploaded: ino=" << c.ino()
                     << " chunk=" << c.index()
                     << " size=" << data.size();
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

  // Remove uploaded chunks.
  dq.erase(std::remove_if(dq.begin(), dq.end(),
                          [](const Chunk& c) { return c.IsSealed(); }),
           dq.end());

  return Status::OK();
}

Status ChunkManager::Release(uint64_t fh) {
  Status status = Flush(fh);
  chunks_.erase(fh);
  return status;
}

}  // namespace swordfs::chunk
