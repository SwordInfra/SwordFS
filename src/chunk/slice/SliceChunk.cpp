// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/slice/SliceChunk.hpp"

#include <folly/io/IOBuf.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "chunk/slice/SliceObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"

namespace swordfs::chunk::slice {

SliceChunk::SliceChunk(metadata::InodeID ino, metadata::ChunkIndex index, size_t chunk_size,
                       std::shared_ptr<metadata::slice::SliceChunkMetadata> slice_metadata, metadata::IMetaEngine *meta,
                       storage::IDataEngine *data)
    : Chunk(index),
      ino_(ino),
      chunk_size_(chunk_size),
      slice_metadata_(std::move(slice_metadata)),
      meta_(meta),
      data_(data) {
}

utils::Status SliceChunk::Write(size_t offset, const folly::IOBuf &data) {
  const size_t len = data.computeChainDataLength();
  if (offset > chunk_size_ || len > chunk_size_ - offset) {
    return utils::Status::InvalidArgument("SliceChunk::Write exceeds logical chunk bounds");
  }
  if (len == 0) {
    return utils::Status::OK();
  }
  auto flattened = data.cloneCoalesced();
  std::string bytes(reinterpret_cast<const char *>(flattened->data()), len);
  const size_t end = offset + len;
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  auto it = dirty_.lower_bound(offset);
  if (it != dirty_.begin()) {
    --it;
  }
  std::vector<std::pair<size_t, std::string>> preserved;
  while (it != dirty_.end() && it->first < end) {
    const size_t start = it->first;
    const size_t old_end = start + it->second.size();
    if (old_end <= offset) {
      ++it;
      continue;
    }
    if (start < offset) {
      preserved.emplace_back(start, it->second.substr(0, offset - start));
    }
    if (old_end > end) {
      preserved.emplace_back(end, it->second.substr(end - start));
    }
    it = dirty_.erase(it);
  }
  dirty_.emplace(offset, std::move(bytes));
  for (auto &part : preserved) {
    dirty_.emplace(part.first, std::move(part.second));
  }
  return utils::Status::OK();
}

utils::Status SliceChunk::Read(size_t offset, size_t len, folly::IOBuf *out) const {
  if (out == nullptr || offset > chunk_size_ || len > chunk_size_ - offset || out->tailroom() < len) {
    return utils::Status::InvalidArgument("SliceChunk::Read invalid buffer or range");
  }
  if (len == 0) {
    return utils::Status::OK();
  }

  // Snapshot only the requested dirty bytes. Do not retain the fiber mutex
  // across metadata or object-store I/O; a concurrent Write sees a later view.
  std::vector<std::pair<size_t, std::string>> overlays;
  std::vector<BoundaryClamp> boundaries;
  std::optional<utils::Status> boundary_error;
  {
    std::shared_lock<utils::FiberRWMutex> lock(mutex_);
    boundaries = boundaries_;
    boundary_error = boundary_error_;
    auto it = dirty_.lower_bound(offset);
    if (it != dirty_.begin()) {
      --it;
    }
    for (; it != dirty_.end() && it->first < offset + len; ++it) {
      const size_t start = std::max(offset, it->first);
      const size_t end = std::min(offset + len, it->first + it->second.size());
      if (start < end) {
        overlays.emplace_back(start - offset, it->second.substr(start - it->first, end - start));
      }
    }
  }

  if (boundary_error.has_value()) {
    // A void Chunk::ClampPublishedPrefix callback cannot propagate a failed
    // source capture. Reject the view instead of guessing which bytes survived.
    return *boundary_error;
  }

  std::string result(len, '\0');
  metadata::FileChunkSnapshot snapshot;
  auto status = meta_->ReadFileChunkSnapshot(ino_, Index(), &snapshot);
  if (!status.ok()) {
    return status;
  }
  if (snapshot.chunk_id.has_value()) {
    if (chunk_size_ > std::numeric_limits<uint32_t>::max()) {
      return utils::Status::InvalidArgument("Slice chunk exceeds 32-bit record range");
    }
    std::vector<SliceRecord> records;
    status = slice_metadata_->ReadRecords(*snapshot.chunk_id, &records);
    if (status.IsNotFound() || (status.ok() && records.empty())) {
      return utils::Status::Malformed("mapped ChunkID is missing authoritative Slice records");
    }
    if (!status.ok()) {
      return status;
    }
    uint64_t start = 0;
    status = metadata::CalculateChunkStartOffset(Index(), chunk_size_, &start);
    if (!status.ok()) {
      return status;
    }
    const size_t size_at_eof =
        snapshot.inode.attr.size > start
            ? static_cast<size_t>(std::min<uint64_t>(chunk_size_, snapshot.inode.attr.size - start))
            : 0;
    const size_t published_length = offset < size_at_eof ? std::min(len, size_at_eof - offset) : 0;
    std::vector<ResolvedSlice> spans;
    // Each sanitation event clips only the records preceding its captured
    // history length. Traverse newest-to-oldest records and epochs once,
    // taking the strictest subsequent boundary for each record.
    if (!boundaries.empty() && boundaries.back().chunk_id == snapshot.chunk_id) {
      if (records.size() < boundaries.back().record_count) {
        return utils::Status::Malformed("Slice history shrank after retained boundary sanitation");
      }
      std::vector<SliceRecord> sanitized;
      sanitized.reserve(records.size());
      size_t boundary_index = boundaries.size();
      size_t visible_prefix = chunk_size_;
      for (size_t i = records.size(); i > 0; --i) {
        while (boundary_index > 0 && boundaries[boundary_index - 1].record_count >= i) {
          visible_prefix = std::min(visible_prefix, boundaries[--boundary_index].visible_prefix);
        }
        auto record = records[i - 1];
        status = ValidateRecord(record, static_cast<uint32_t>(chunk_size_));
        if (!status.ok()) {
          return status;
        }
        if (record.pos >= visible_prefix) {
          continue;
        }
        record.len = static_cast<uint32_t>(std::min<size_t>(record.len, visible_prefix - record.pos));
        sanitized.push_back(record);
      }
      std::reverse(sanitized.begin(), sanitized.end());
      status = ResolveRange(sanitized, static_cast<uint32_t>(chunk_size_), static_cast<uint32_t>(offset),
                            static_cast<uint32_t>(published_length), &spans);
    } else {
      status = ResolveRange(records, static_cast<uint32_t>(chunk_size_), static_cast<uint32_t>(offset),
                            static_cast<uint32_t>(published_length), &spans);
    }
    if (!status.ok()) {
      return status;
    }
    // Read only published bytes not already overridden by accepted local
    // writes. Both collections are sorted, so one cursor skips old intervals
    // without scanning all dirty ranges for every published span.
    size_t overlay_index = 0;
    for (const auto &span : spans) {
      if (span.id == SliceID()) {
        continue;
      }
      const size_t span_start = span.pos;
      const size_t span_end = span_start + span.len;
      while (overlay_index < overlays.size() &&
             offset + overlays[overlay_index].first + overlays[overlay_index].second.size() <= span_start) {
        ++overlay_index;
      }
      const auto key = SliceObjectKey(span.id);
      auto fetch = [&](size_t start_pos, size_t end_pos) -> utils::Status {
        if (start_pos == end_pos) {
          return utils::Status::OK();
        }
        const size_t bytes = end_pos - start_pos;
        auto physical = folly::IOBuf::create(bytes);
        auto status = data_->Get(key, span.off + (start_pos - span_start), bytes, physical.get());
        if (!status.ok()) {
          return status;
        }
        if (physical->length() != bytes) {
          return utils::Status::IOError("SliceChunk::Read: short object range");
        }
        std::memcpy(result.data() + start_pos - offset, physical->data(), bytes);
        return utils::Status::OK();
      };

      size_t cursor = span_start;
      for (size_t i = overlay_index; i < overlays.size() && cursor < span_end; ++i) {
        const size_t dirty_start = offset + overlays[i].first;
        const size_t dirty_end = dirty_start + overlays[i].second.size();
        if (dirty_start >= span_end) {
          break;
        }
        if (dirty_end <= cursor) {
          continue;
        }
        if (dirty_start > cursor) {
          status = fetch(cursor, dirty_start);
          if (!status.ok()) {
            return status;
          }
        }
        cursor = std::min(span_end, dirty_end);
      }
      status = fetch(cursor, span_end);
      if (!status.ok()) {
        return status;
      }
    }
  }
  for (const auto &[relative, bytes] : overlays) {
    std::memcpy(result.data() + relative, bytes.data(), bytes.size());
  }
  // Only publish the result after all metadata and physical reads succeed.
  std::memcpy(out->writableTail(), result.data(), len);
  out->append(len);
  return utils::Status::OK();
}

utils::Status SliceChunk::Flush() {
  return utils::Status::NotSupported("Slice flush requires durable writer and publication (#485/#271)");
}

void SliceChunk::TruncateLocal(size_t size) {
  CaptureBoundary(size);
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  auto it = dirty_.lower_bound(size);
  dirty_.erase(it, dirty_.end());
  if (!dirty_.empty()) {
    it = std::prev(dirty_.end());
    if (it->first + it->second.size() > size) {
      it->second.resize(size - it->first);
    }
  }
}

void SliceChunk::ClampPublishedPrefix(size_t visible_prefix) {
  CaptureBoundary(visible_prefix);
}

void SliceChunk::CaptureBoundary(size_t visible_prefix) {
  metadata::FileChunkSnapshot snapshot;
  auto status = meta_->ReadFileChunkSnapshot(ino_, Index(), &snapshot);
  size_t count = 0;
  if (status.ok() && snapshot.chunk_id.has_value()) {
    std::vector<SliceRecord> records;
    status = slice_metadata_->ReadRecords(*snapshot.chunk_id, &records);
    if (status.IsNotFound() || (status.ok() && records.empty())) {
      status = utils::Status::Malformed("mapped ChunkID has no records when capturing Slice boundary");
    }
    if (status.ok()) {
      count = records.size();
    }
  }
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  if (!status.ok()) {
    boundary_error_ = status;
    return;
  }
  boundary_error_.reset();
  if (!boundaries_.empty() && boundaries_.back().chunk_id != snapshot.chunk_id) {
    boundaries_.clear();
  }
  visible_prefix = std::min(visible_prefix, chunk_size_);
  if (!boundaries_.empty() && count < boundaries_.back().record_count) {
    boundary_error_ = utils::Status::Malformed("Slice history shrank during repeated sanitation");
    return;
  }
  if (!boundaries_.empty() && count == boundaries_.back().record_count) {
    boundaries_.back().visible_prefix = std::min(visible_prefix, boundaries_.back().visible_prefix);
  } else {
    boundaries_.push_back(
        BoundaryClamp{.visible_prefix = visible_prefix, .chunk_id = snapshot.chunk_id, .record_count = count});
  }
}

bool SliceChunk::HasPendingWrites() const {
  std::shared_lock<utils::FiberRWMutex> lock(mutex_);
  return !dirty_.empty();
}

}  // namespace swordfs::chunk::slice
