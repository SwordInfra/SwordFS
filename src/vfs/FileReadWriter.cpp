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
    // 1. Wait for every fiber to finish.  Each fiber lambda dereferences
    //    |raw| (a raw pointer into the matching Pending), so we must let
    //    all of them complete before this object — and |ops_| — can be
    //    destroyed.  Returning early after the first failure would leave
    //    in-flight fibers writing into freed memory.
    for (auto &p : ops_) {
      p->baton.wait();
    }
    // 2. Now it is safe to inspect status / accumulate bytes.
    for (auto &p : ops_) {
      if (!p->status.ok()) {
        return p->status;
      }
      total_ += p->bytes;
    }
    return Status::OK();
  }

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
// FileChunkManager
// ────────────────────────────────────────────────────────────────

chunk::Chunk *FileChunkManager::Get(metadata::ChunkIndex idx, bool create_if_missing) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = chunks_.find(idx);
  if (it != chunks_.end()) {
    return &it->second;
  }
  // Not cached — try lazy-load from metadata engine.
  auto c = chunk::Chunk(ino_, idx);
  auto status = c.Initialize();
  if (!status.ok()) {
    return nullptr;
  } else if (c.IsFlushed() || create_if_missing) {
    it = chunks_.try_emplace(idx, std::move(c)).first;
    return &it->second;
  }
  return nullptr;
}

chunk::Chunk *FileChunkManager::GetNextFlushable() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &[idx, c] : chunks_) {
    if (c.Flushable()) {
      c.Seal();
      return &c;
    }
  }
  return nullptr;
}

void FileChunkManager::Truncate(metadata::ChunkIndex new_last_idx,
                                std::vector<metadata::ChunkIndex> *dropped) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto it = chunks_.begin(); it != chunks_.end();) {
    if (it->first >= new_last_idx) {
      if (dropped) {
        dropped->push_back(it->first);
      }
      it = chunks_.erase(it);
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
      data_(volume::VolumeImpl::Instance().data_engine()),
      chunks_(ino) {}

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

    auto *c = chunks_.Get(idx, /*create_if_missing=*/true);
    if (!c) {
      return utils::Status::Internal("FileReadWriter::Write: failed to get chunk");
    }

    size_t room = chunk_size_ - (cur_off % chunk_size_);
    size_t n = std::min(remaining, room);
    auto slice = folly::IOBuf::takeOwnership(
        const_cast<uint8_t *>(buf.data()) + (cur_off - off), n, n,
        +[](void *, void *) {}, nullptr, false);
    auto status = c->Write(cur_off, *slice);
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileReadWriter::Write FAILED: ino=" << ino_
                        << " off=" << cur_off
                        << " chunk=" << c->index() << " — "
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
  MultiChunkReadWriter multi;
  size_t remaining = size;
  off_t cur_off = off;
  auto *const write_start = out->writableData();

  while (remaining > 0) {
    // 1) Try the unified chunk map (dirty + flushed).
    metadata::ChunkIndex idx = static_cast<metadata::ChunkIndex>(
        cur_off / static_cast<off_t>(chunk_size_));
    auto *c = chunks_.Get(idx, /*create_if_missing=*/false);

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
      CHECK(window_cap > 0) << "window_cap=0: cur_off=" << cur_off
                            << " DataEnd=" << c->DataEnd()
                            << " remaining=" << remaining;

      auto window = folly::IOBuf::takeOwnership(
          write_start + static_cast<size_t>(cur_off - off),
          window_cap, static_cast<std::size_t>(0),
          +[](void *, void *) {}, nullptr, false);

      multi.SubmitRead(c, chunk_off, window_cap, std::move(window));
      remaining -= window_cap;
      cur_off += static_cast<off_t>(window_cap);
      continue;
    }

    // 2) Hole — fill with zeros up to the next chunk boundary.
    size_t hole =
        std::min(remaining,
                 chunk_size_ - static_cast<size_t>(cur_off % chunk_size_));
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

// ────────────────────────────────────────────────────────────────
// Flush
// ────────────────────────────────────────────────────────────────

utils::Status FileReadWriter::Flush() {
  off_t file_end = 0;
  while (auto *c = chunks_.GetNextFlushable()) {
    auto idx = c->index();
    auto status = c->Flush();
    if (!status.ok()) {
      SWORDFS_LOG_ERROR << "FileReadWriter::Flush chunk FAILED: ino=" << ino_
                        << " chunk=" << idx
                        << " — " << status.message();
      continue;
    }
    if (c->DataEnd() > file_end) {
      file_end = c->DataEnd();
    }
    // Chunk stays in the map with kFlushed state — future reads
    // will route through Chunk::Read() → data_->Get().
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

utils::Status FileReadWriter::Truncate(size_t size) {
  auto status = meta_->Truncate(ino_, size);
  if (!status.ok()) {
    return status;
  }
  // Drop cached chunks at or beyond the new last chunk.  The chunk
  // containing the truncated offset (if any) is kept and will be
  // re-loaded from metadata on next access.  Chunks below it remain
  // so reads can hit them directly.
  std::vector<metadata::ChunkIndex> dropped;
  if (chunk_size_ > 0) {
    // The first chunk index beyond the truncated file size; cached
    // chunks at or past this index are dropped.
    const auto new_last_idx =
        static_cast<metadata::ChunkIndex>((size + chunk_size_ - 1) / chunk_size_);
    chunks_.Truncate(new_last_idx, &dropped);
  } else {
    chunks_.Truncate(0, &dropped);
  }

  // Drop the now-orphaned chunk objects from the data engine. The
  // metadata side already pruned its chunk map via meta_->Truncate(),
  // but the actual S3 / object-storage objects would otherwise leak
  // until a future GC pass picks them up. Failures are logged but not
  // propagated — a missing chunk object is recoverable on next access
  // (read of that chunk index returns NotFound), whereas a partial
  // metadata truncate would corrupt the file size view.
  if (data_ && !dropped.empty()) {
    for (const auto idx : dropped) {
      const auto key = utils::ChunkKey(ino_, idx);
      auto st = data_->Delete(key);
      if (!st.ok()) {
        SWORDFS_LOG_ERROR << "FileReadWriter::Truncate: data->Delete("
                          << key << ") failed: " << st.message();
      }
    }
  }

  return utils::Status::OK();
}

}  // namespace swordfs::vfs