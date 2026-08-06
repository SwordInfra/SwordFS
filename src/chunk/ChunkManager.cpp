// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/ChunkManager.hpp"

#include <folly/io/IOBuf.h>

#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Logging.hpp"

#define FUSE_USE_VERSION 312
#include <fuse_lowlevel.h>

namespace swordfs::chunk {

ChunkManager &ChunkManager::Instance() {
  static ChunkManager instance;
  return instance;
}

void ChunkManager::Initialize(metadata::IMetaEngine *meta,
                              storage::IDataEngine *data,
                              size_t chunk_size) {
  std::unique_lock lock(mutex_);
  meta_ = meta;
  data_ = data;
  chunk_size_ = chunk_size;
  chunks_.clear();
}

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

std::deque<Chunk> &ChunkManager::GetChunks(uint64_t fh) {
  auto it = chunks_.find(fh);
  if (it == chunks_.end()) {
    it = chunks_.emplace(fh, std::deque<Chunk>{}).first;
  }
  return it->second;
}

Chunk *ChunkManager::FindChunk(InodeID ino, off_t off) {
  std::shared_lock lock(mutex_);
  for (auto &[fh, dq] : chunks_) {
    if (dq.empty()) continue;
    if (dq.front().ino() != ino) continue;
    for (auto &c : dq) {
      if (off >= c.StartOffset() && off < c.EndOffset()) return &c;
    }
  }
  return nullptr;
}

// ────────────────────────────────────────────────────────────────
// Write path
// ────────────────────────────────────────────────────────────────

Chunk &ChunkManager::GetOrCreateChunk(uint64_t fh, InodeID ino,
                                      off_t off) {
  std::unique_lock lock(mutex_);
  auto &dq = GetChunks(fh);

  // Find or create the chunk that covers |off|.
  uint32_t idx = static_cast<uint32_t>(
      off / static_cast<off_t>(chunk_size_));

  // Create missing intermediate chunks.
  while (static_cast<uint32_t>(dq.size()) <= idx) {
    uint32_t next = dq.empty() ? 0 : dq.back().index() + 1;
    dq.emplace_back(ino, next, chunk_size_, data_);
  }

  return dq[idx];
}

Status ChunkManager::Flush(uint64_t fh) {
  // Find the deque under lock, then release it before doing I/O.
  decltype(chunks_)::iterator it;
  InodeID ino;  // captured under lock before releasing for I/O
  {
    std::shared_lock lock(mutex_);
    it = chunks_.find(fh);
    if (it == chunks_.end()) return Status::OK();
    ino = it->second.front().ino();
  }

  auto &dq = it->second;
  off_t file_end = 0;
  for (auto &c : dq) {
    if (c.EndOffset() > file_end) file_end = c.EndOffset();
    auto status = c.Flush();
    if (!status.ok()) return status;
  }

  // Update file size.
  if (file_end > 0) {
    struct stat attr;
    if (meta_->GetAttr(ino, &attr).ok() &&
        file_end > attr.st_size) {
      struct stat new_attr = {};
      new_attr.st_size = file_end;
      meta_->SetAttr(ino, &new_attr, FUSE_SET_ATTR_SIZE, nullptr);
    }
  }

  return Status::OK();
}

Status ChunkManager::Release(uint64_t fh) {
  Status status = Flush(fh);
  std::unique_lock lock(mutex_);
  chunks_.erase(fh);
  return status;
}

}  // namespace swordfs::chunk
