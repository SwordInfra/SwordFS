// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/slice/SliceChunk.hpp"

#include <folly/io/IOBuf.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "chunk/slice/SliceObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "storage/IDataEngine.hpp"

namespace swordfs::chunk::slice {

// Candidate state is intentionally not exposed by the public Chunk header.
// Put completion alone does not authorize retirement of any dirty bytes.
struct SliceChunk::FrozenSlice {
  size_t pos;
  std::string payload;
  size_t visible_len;
  // Physical preparation only. #271 owns the typed SliceRecord consumer,
  // atomic publication and the final ownership transition.
  std::optional<SliceID> prepared_id;
};

namespace {

using DirtyRanges = std::map<size_t, std::string>;
constexpr size_t kMaxActiveSliceBytes = size_t{4} * 1024 * 1024;

// Merge only touching written intervals; do not turn a sparse gap into a
// zero-padded object that would hide already published bytes on commit.
size_t AcceptWrite(DirtyRanges &ranges, size_t offset, std::string_view bytes) {
  size_t start = offset;
  size_t end = offset + bytes.size();
  auto it = ranges.lower_bound(offset);
  if (it == ranges.end() || it->first > end) {
    if (it == ranges.begin() || std::prev(it)->first + std::prev(it)->second.size() < offset) {
      ranges.emplace(offset, std::string(bytes));
      return bytes.size();
    }
  }
  if (it != ranges.begin()) {
    auto previous = std::prev(it);
    const size_t previous_end = previous->first + previous->second.size();
    if (previous_end >= offset && (it == ranges.end() || it->first > std::max(end, previous_end))) {
      // Overwrite and contiguous-tail extension avoid repeatedly copying the
      // existing prefix for each small Write (quadratic sequential writes).
      if (end <= previous_end) {
        std::memcpy(previous->second.data() + offset - previous->first, bytes.data(), bytes.size());
        return 0;
      }
      previous->second.resize(offset - previous->first);
      previous->second.append(bytes);
      return end - previous_end;
    }
    it = previous;
  }
  std::vector<std::pair<size_t, std::string>> previous;
  size_t previous_bytes = 0;
  while (it != ranges.end()) {
    const size_t old_end = it->first + it->second.size();
    if (old_end < start) {
      ++it;
      continue;
    }
    if (it->first > end) {
      break;
    }
    start = std::min(start, it->first);
    end = std::max(end, old_end);
    previous_bytes += it->second.size();
    previous.emplace_back(it->first, std::move(it->second));
    it = ranges.erase(it);
  }
  std::string merged(end - start, '\0');
  for (const auto &[old_start, old_bytes] : previous) {
    std::memcpy(merged.data() + old_start - start, old_bytes.data(), old_bytes.size());
  }
  std::memcpy(merged.data() + offset - start, bytes.data(), bytes.size());
  const size_t newly_covered = merged.size() - previous_bytes;
  ranges.emplace(start, std::move(merged));
  return newly_covered;
}

void ClipRanges(DirtyRanges &ranges, size_t size) {
  auto it = ranges.lower_bound(size);
  ranges.erase(it, ranges.end());
  if (!ranges.empty()) {
    it = std::prev(ranges.end());
    if (it->first + it->second.size() > size) {
      it->second.resize(size - it->first);
    }
  }
}

}  // namespace

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
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  // This projection includes every unpublished frozen Slice plus every newer
  // active Write. An older Put completion must never clear newer local bytes.
  AcceptWrite(dirty_, offset, bytes);
  // Keep the mutable working set bounded even for a single large Write.
  // Frozen-but-unpublished bytes still count toward #202's future budget.
  for (size_t cursor = 0; cursor < bytes.size();) {
    const size_t part_size = std::min(kMaxActiveSliceBytes, bytes.size() - cursor);
    const size_t part_start = offset + cursor;
    const size_t part_end = part_start + part_size;
    if (active_bytes_ + part_size > kMaxActiveSliceBytes) {
      // Account actual new coverage. Repeated overwrites of already-buffered
      // bytes should not force a new Slice merely because its range is large.
      size_t covered = 0;
      auto it = active_.upper_bound(part_start);
      if (it != active_.begin()) {
        --it;
      }
      for (; it != active_.end() && it->first < part_end; ++it) {
        const size_t first = std::max(it->first, part_start);
        const size_t last = std::min(it->first + it->second.size(), part_end);
        if (first < last) {
          covered += last - first;
        }
      }
      if (active_bytes_ + part_size - covered > kMaxActiveSliceBytes) {
        FreezeActiveLocked();
      }
    }
    active_bytes_ += AcceptWrite(active_, part_start, std::string_view(bytes).substr(cursor, part_size));
    if (active_bytes_ >= kMaxActiveSliceBytes) {
      FreezeActiveLocked();
    }
    cursor += part_size;
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

  // dirty_ is the merged Active + Frozen projection in acceptance order.
  // Snapshot requested bytes before metadata/S3 I/O; neither freeze nor Put
  // success makes pending dirty data temporarily disappear from this view.
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

void SliceChunk::FreezeActiveLocked() {
  if (active_.empty()) {
    return;
  }
  for (auto &[pos, bytes] : active_) {
    const size_t len = bytes.size();
    frozen_.push_back(std::make_shared<FrozenSlice>(
        FrozenSlice{.pos = pos, .payload = std::move(bytes), .visible_len = len, .prepared_id = std::nullopt}));
  }
  active_.clear();
  active_bytes_ = 0;
}

utils::Status SliceChunk::Flush() {
  if (chunk_size_ > std::numeric_limits<uint32_t>::max()) {
    return utils::Status::InvalidArgument("Slice flush exceeds 32-bit record range");
  }
  std::vector<std::shared_ptr<FrozenSlice>> candidates;
  {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    if (flushing_) {
      return utils::Status::Busy("Slice flush preparation already in flight");
    }
    flushing_ = true;
    FreezeActiveLocked();
    candidates = frozen_;
  }

  const auto finish = [&](utils::Status status) {
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    flushing_ = false;
    return status;
  };

  // One flight per Chunk retains local acceptance order independent of later
  // writes. An uncertain Put is retried only with a fresh physical identity;
  // already prepared objects are never overwritten or uploaded twice.
  for (const auto &candidate : candidates) {
    {
      std::shared_lock<utils::FiberRWMutex> lock(mutex_);
      if (candidate->visible_len == 0 || candidate->prepared_id.has_value()) {
        continue;
      }
    }
    SliceID id;
    auto status = slice_metadata_->AllocateSliceID(&id);
    if (!status.ok()) {
      return finish(status);
    }
    if (id == SliceID() || id.Value() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return finish(utils::Status::Malformed("Slice allocator returned an invalid physical identity"));
    }
    // Frozen payload storage is never mutated or removed during in-flight
    // preparation, even if a concurrent local truncate clips its visibility.
    status = data_->Put(SliceObjectKey(id), folly::IOBuf::copyBuffer(candidate->payload));
    if (!status.ok()) {
      return finish(status);
    }
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    // TruncateLocal may have retired this candidate while Put was in flight.
    // Its shared_ptr snapshot keeps the payload alive through Put, but a
    // completely invisible object must never become eligible for publication.
    if (candidate->visible_len != 0) {
      candidate->prepared_id = id;
    }
  }
  // Physical preparation alone is never a successful durable Flush. #271
  // must publish the ordered candidates, FileMetadata and EOF atomically.
  return finish(utils::Status::NotSupported("Slice records require authoritative publication (#271)"));
}

void SliceChunk::TruncateLocal(size_t size) {
  CaptureBoundary(size);
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  ClipRanges(dirty_, size);
  ClipRanges(active_, size);
  active_bytes_ = 0;
  for (const auto &[pos, bytes] : active_) {
    active_bytes_ += bytes.size();
  }
  for (auto &candidate : frozen_) {
    const size_t remaining = size > candidate->pos ? size - candidate->pos : 0;
    candidate->visible_len = std::min(candidate->visible_len, remaining);
  }
  // Fully clipped generations have no publishable data. Release Chunk
  // ownership immediately; any in-flight Put still owns its local snapshot.
  // Do not delete a possibly completed but unreferenced immutable object.
  std::erase_if(frozen_, [](const auto &candidate) { return candidate->visible_len == 0; });
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
