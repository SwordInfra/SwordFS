// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/slice/SliceChunk.hpp"

#include <folly/container/F14Set.h>
#include <folly/io/IOBuf.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_set>
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
  // Logical record length after local truncation. A fixed-window RMW
  // candidate may carry committed bytes beyond eof_len; those bytes are read
  // back only through the window record and never extend FileMetadata EOF.
  size_t visible_len;
  // Dirty extent from pos, in bytes. For sparse candidates this equals
  // visible_len. For a full-window RMW candidate it ends at the last dirty
  // byte, not at the 1MiB window boundary.
  size_t eof_len;
  // True once PrepareWindowCandidates has materialized this candidate.
  // Prepared window candidates are never regrouped with newer dirty ranges.
  bool window_group = false;
  std::optional<SliceID> prepared_id;
  // An existing-index append is a separate authority from FileMetadata
  // finalization. Retain its identity across a definite metadata conflict
  // so a normal retry does not blindly RPUSHX a duplicate.
  std::optional<metadata::ChunkID> recorded_chunk_id;
  // Initial attach prepares the nonempty private index before publishing
  // its ChunkID; this unattached candidate may become orphan garbage.
  std::optional<metadata::ChunkID> created_chunk_id;
  // Unknown Redis replies must be read back, not blindly replayed.
  std::optional<metadata::ChunkID> uncertain_record_chunk_id;
};

namespace {

using DirtyRanges = std::map<size_t, std::string>;
constexpr size_t kMaxActiveSliceBytes = size_t{4} * 1024 * 1024;
constexpr size_t kRmwWindowBytes = size_t{1} * 1024 * 1024;
constexpr int kMaxRmwInstallAttempts = 4;

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

utils::Status SliceChunk::ReadCommittedRange(size_t offset, size_t len, std::string *out) const {
  if (out == nullptr || offset > chunk_size_ || len > chunk_size_ - offset) {
    return utils::Status::InvalidArgument("SliceChunk::ReadCommittedRange invalid range");
  }
  if (chunk_size_ > std::numeric_limits<uint32_t>::max()) {
    return utils::Status::InvalidArgument("Slice chunk exceeds 32-bit record range");
  }
  out->assign(len, '\0');
  if (len == 0) {
    return utils::Status::OK();
  }

  std::vector<BoundaryClamp> boundaries;
  std::optional<utils::Status> boundary_error;
  {
    std::shared_lock<utils::FiberRWMutex> lock(mutex_);
    boundaries = boundaries_;
    boundary_error = boundary_error_;
  }
  if (boundary_error.has_value()) {
    return *boundary_error;
  }

  metadata::FileChunkSnapshot snapshot;
  auto status = meta_->ReadFileChunkSnapshot(ino_, Index(), &snapshot);
  if (!status.ok()) {
    return status;
  }
  if (!snapshot.chunk_id.has_value()) {
    return utils::Status::OK();
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
  if (offset >= size_at_eof) {
    return utils::Status::OK();
  }
  const size_t committed_length = std::min(len, size_at_eof - offset);
  std::vector<ResolvedSlice> spans;
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
                          static_cast<uint32_t>(committed_length), &spans);
  } else {
    status = ResolveRange(records, static_cast<uint32_t>(chunk_size_), static_cast<uint32_t>(offset),
                          static_cast<uint32_t>(committed_length), &spans);
  }
  if (!status.ok()) {
    return status;
  }
  for (const auto &span : spans) {
    if (span.id == SliceID()) {
      continue;
    }
    auto physical = folly::IOBuf::create(span.len);
    status = data_->Get(SliceObjectKey(span.id), span.off, span.len, physical.get());
    if (!status.ok()) {
      return status;
    }
    if (physical->length() != span.len) {
      return utils::Status::IOError("SliceChunk::ReadCommittedRange: short object range");
    }
    std::memcpy(out->data() + span.pos - offset, physical->data(), span.len);
  }
  return utils::Status::OK();
}

void SliceChunk::FreezeActiveLocked() {
  if (active_.empty()) {
    return;
  }
  for (auto &[pos, bytes] : active_) {
    const size_t len = bytes.size();
    frozen_.push_back(std::make_shared<FrozenSlice>(FrozenSlice{.pos = pos,
                                                                .payload = std::move(bytes),
                                                                .visible_len = len,
                                                                .eof_len = len,
                                                                .window_group = false,
                                                                .prepared_id = std::nullopt}));
  }
  active_.clear();
  active_bytes_ = 0;
}

utils::Status SliceChunk::PrepareWindowCandidates(std::vector<std::shared_ptr<FrozenSlice>> *candidates) {
  // Lengths can be clipped by TruncateLocal while committed reads are in
  // flight. Snapshot every mutable length under the lock; payload storage is
  // immutable after FreezeActiveLocked and remains owned by the candidate.
  struct PendingView {
    std::shared_ptr<FrozenSlice> candidate;
    size_t visible_len;
    size_t eof_len;
  };
  struct GroupBuild {
    size_t pos = 0;
    size_t record_end = 0;
    size_t dirty_end = 0;
    size_t insert_after = 0;
    size_t interval_count = 0;
    std::shared_ptr<FrozenSlice> candidate;
  };

  for (int attempt = 0; attempt < kMaxRmwInstallAttempts; ++attempt) {
    std::vector<PendingView> pending;
    size_t epoch = 0;
    {
      std::shared_lock<utils::FiberRWMutex> lock(mutex_);
      epoch = truncate_epoch_;
      pending.reserve(frozen_.size());
      for (const auto &candidate : frozen_) {
        pending.push_back(
            PendingView{.candidate = candidate, .visible_len = candidate->visible_len, .eof_len = candidate->eof_len});
      }
    }

    std::unordered_set<const FrozenSlice *> sources;
    std::map<size_t, GroupBuild> groups;
    for (size_t i = 0; i < pending.size(); ++i) {
      const auto &view = pending[i];
      if (view.visible_len == 0 || view.candidate->window_group || view.candidate->prepared_id.has_value() ||
          view.candidate->created_chunk_id.has_value() || view.candidate->recorded_chunk_id.has_value() ||
          view.candidate->uncertain_record_chunk_id.has_value()) {
        // A candidate whose physical or metadata preparation already began
        // retains its own retry identity. A newer RMW group may still overlay
        // its bytes, but the prepared candidate itself is never abandoned.
        continue;
      }
      // Never split a dense contiguous Slice into artificial 1 MiB objects;
      // only small dirty ranges entirely inside a window may be coalesced.
      const size_t window_base = (view.candidate->pos / kRmwWindowBytes) * kRmwWindowBytes;
      if (view.visible_len >= kRmwWindowBytes ||
          view.visible_len > window_base + kRmwWindowBytes - view.candidate->pos) {
        continue;
      }
      const size_t cursor = view.candidate->pos;
      const size_t portion_end = cursor + view.visible_len;
      auto [it, inserted] = groups.try_emplace(window_base);
      auto &group = it->second;
      if (inserted) {
        group.pos = cursor;
        group.record_end = portion_end;
        group.insert_after = i;
      } else {
        group.pos = std::min(group.pos, cursor);
        group.record_end = std::max(group.record_end, portion_end);
      }
      ++group.interval_count;
      group.dirty_end = std::max(group.dirty_end, portion_end);
      group.insert_after = std::max(group.insert_after, i);
    }
    // One contiguous interval is already a valid immutable Slice, with no
    // existing gap to hydrate. Leave it untouched: avoid unnecessary S3 GET
    // or data amplification for isolated small writes.
    std::erase_if(groups, [](const auto &entry) { return entry.second.interval_count < 2; });
    for (const auto &view : pending) {
      if (view.visible_len == 0 || view.candidate->window_group || view.candidate->prepared_id.has_value() ||
          view.candidate->created_chunk_id.has_value() || view.candidate->recorded_chunk_id.has_value() ||
          view.candidate->uncertain_record_chunk_id.has_value()) {
        continue;
      }
      const size_t window_base = (view.candidate->pos / kRmwWindowBytes) * kRmwWindowBytes;
      if (view.visible_len < kRmwWindowBytes &&
          view.visible_len <= window_base + kRmwWindowBytes - view.candidate->pos && groups.contains(window_base)) {
        sources.insert(view.candidate.get());
      }
    }
    if (sources.empty()) {
      return utils::Status::OK();
    }

    std::vector<size_t> group_order;
    group_order.reserve(groups.size());
    for (const auto &[base, group] : groups) {
      (void)group;
      group_order.push_back(base);
    }
    for (const size_t base : group_order) {
      auto &group = groups.at(base);
      std::string payload;
      auto status = ReadCommittedRange(group.pos, group.record_end - group.pos, &payload);
      if (!status.ok()) {
        return status;
      }
      // Overlay every pending generation in acceptance order. This includes
      // older prepared groups whose records will publish before this one; the
      // group is inserted after them below so later local bytes still win.
      for (size_t i = 0; i < pending.size(); ++i) {
        const auto &view = pending[i];
        if (view.visible_len == 0) {
          continue;
        }
        const size_t candidate_start = view.candidate->pos;
        const size_t candidate_end = candidate_start + view.visible_len;
        const size_t start = std::max(group.pos, candidate_start);
        const size_t end = std::min(group.record_end, candidate_end);
        if (start >= end) {
          continue;
        }
        std::memcpy(payload.data() + start - group.pos, view.candidate->payload.data() + start - candidate_start,
                    end - start);
        const size_t dirty_end = candidate_start + std::min(view.eof_len, view.visible_len);
        if (dirty_end > group.pos) {
          group.dirty_end = std::max(group.dirty_end, std::min(dirty_end, group.record_end));
        }
        group.insert_after = std::max(group.insert_after, i);
      }
      group.dirty_end = std::clamp(group.dirty_end, group.pos, group.record_end);
      const size_t record_len = payload.size();
      const size_t eof_len = group.dirty_end - group.pos;
      group.candidate = std::make_shared<FrozenSlice>(FrozenSlice{.pos = group.pos,
                                                                  .payload = std::move(payload),
                                                                  .visible_len = record_len,
                                                                  .eof_len = eof_len,
                                                                  .window_group = true,
                                                                  .prepared_id = std::nullopt});
    }

    std::map<size_t, std::vector<std::shared_ptr<FrozenSlice>>> insert_after;
    for (const size_t base : group_order) {
      const auto &group = groups.at(base);
      insert_after[group.insert_after].push_back(group.candidate);
    }

    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    if (epoch != truncate_epoch_) {
      // The local dirty view was clipped while committed bytes were read.
      // Rebuild from the current, smaller source set rather than publishing
      // a record past the truncate boundary.
      continue;
    }
    std::vector<std::shared_ptr<FrozenSlice>> rebuilt;
    rebuilt.reserve(frozen_.size());
    for (size_t i = 0; i < frozen_.size(); ++i) {
      if (!sources.contains(frozen_[i].get())) {
        rebuilt.push_back(frozen_[i]);
      }
      if (auto it = insert_after.find(i); it != insert_after.end()) {
        rebuilt.insert(rebuilt.end(), it->second.begin(), it->second.end());
      }
    }
    frozen_ = std::move(rebuilt);

    std::vector<std::shared_ptr<FrozenSlice>> ordered;
    ordered.reserve(pending.size());
    for (size_t i = 0; i < pending.size(); ++i) {
      if (!sources.contains(pending[i].candidate.get())) {
        ordered.push_back(pending[i].candidate);
      }
      if (auto it = insert_after.find(i); it != insert_after.end()) {
        ordered.insert(ordered.end(), it->second.begin(), it->second.end());
      }
    }
    *candidates = std::move(ordered);
    return utils::Status::OK();
  }
  return utils::Status::Busy("Slice RMW preparation raced repeated local truncation");
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

  // Consolidate fragmented dirty bytes inside a 1 MiB cache window before
  // normal immutable Slice publication. This is not another Chunk format.
  auto status = PrepareWindowCandidates(&candidates);
  if (!status.ok()) {
    return finish(status);
  }

  // One flight per Chunk retains local acceptance order independent of later
  // writes. An uncertain Put is retried only with a fresh physical identity;
  // already prepared objects are never overwritten or uploaded twice.
  std::vector<std::shared_ptr<FrozenSlice>> committed;
  // Retire in one batch rather than rebuilding all pending dirty ranges
  // after every Slice append (quadratic work for a large fragmented Flush).
  const auto retire_committed = [&] {
    if (committed.empty()) {
      return;
    }
    folly::F14FastSet<const FrozenSlice *> committed_addresses;
    committed_addresses.reserve(committed.size());
    for (const auto &candidate : committed) {
      committed_addresses.insert(candidate.get());
    }
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    std::erase_if(frozen_, [&](const auto &candidate) { return committed_addresses.contains(candidate.get()); });
    RebuildDirtyLocked();
  };
  // One immutable object per prepared Slice. There is no alternative sparse
  // packing format and no experimental publication strategy in production.
  for (const auto &candidate : candidates) {
    {
      std::shared_lock<utils::FiberRWMutex> lock(mutex_);
      if (candidate->visible_len == 0 || candidate->prepared_id.has_value()) {
        continue;
      }
    }
    if (candidate->payload.empty() || candidate->payload.size() > std::numeric_limits<uint32_t>::max()) {
      return finish(utils::Status::InvalidArgument("invalid prepared Slice payload"));
    }
    SliceID id;
    auto status = slice_metadata_->AllocateSliceID(&id);
    if (!status.ok()) {
      return finish(status);
    }
    if (id == SliceID() || id.Value() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return finish(utils::Status::Malformed("Slice allocator returned an invalid physical identity"));
    }
    status = data_->Put(SliceObjectKey(id), folly::IOBuf::copyBuffer(candidate->payload));
    if (!status.ok()) {
      return finish(status);
    }
    std::lock_guard<utils::FiberRWMutex> lock(mutex_);
    if (candidate->visible_len != 0) {
      candidate->prepared_id = id;
    }
  }
  for (const auto &candidate : candidates) {
    {
      std::shared_lock<utils::FiberRWMutex> lock(mutex_);
      if (candidate->visible_len == 0) {
        continue;
      }
    }
    auto status = PublishFrozen(candidate);
    if (!status.ok()) {
      // A partially completed file Flush is allowed to leave previously
      // confirmed Slice publications; never replay them on the next attempt.
      retire_committed();
      return finish(status);
    }
    committed.push_back(candidate);
  }
  // A newer overlapping Active Write may have arrived during immutable PUT
  // or Redis publication. Only the confirmed earlier generation is retired;
  // rebuilding from the remaining generations preserves the newest bytes.
  retire_committed();
  return finish(utils::Status::OK());
}

utils::Status SliceChunk::PublishFrozen(const std::shared_ptr<FrozenSlice> &candidate) {
  // FileReadWriter holds a shared operation barrier during Flush; its
  // exclusive Truncate path cannot invalidate this candidate. The local
  // mutation lock still protects the direct Chunk test/runtime surface.
  SliceRecord record;
  size_t eof_len = 0;
  {
    std::shared_lock<utils::FiberRWMutex> lock(mutex_);
    if (candidate->visible_len == 0) {
      return utils::Status::OK();
    }
    if (!candidate->prepared_id.has_value()) {
      return utils::Status::Internal("Slice physical object is not prepared");
    }
    record =
        SliceRecord{static_cast<uint32_t>(candidate->pos), *candidate->prepared_id,
                    static_cast<uint32_t>(candidate->payload.size()), 0, static_cast<uint32_t>(candidate->visible_len)};
    eof_len = std::min(candidate->eof_len, candidate->visible_len);
  }
  auto status = ValidateRecord(record, static_cast<uint32_t>(chunk_size_));
  if (!status.ok()) {
    return status;
  }
  uint64_t start = 0;
  status = metadata::CalculateChunkStartOffset(Index(), chunk_size_, &start);
  if (!status.ok()) {
    return status;
  }
  if (record.pos > metadata::kMaxSupportedFileSize - start ||
      record.len > metadata::kMaxSupportedFileSize - (start + record.pos)) {
    return utils::Status::InvalidArgument("Slice published record exceeds supported file size");
  }
  // The physical record can cover clean/committed bytes for a full window.
  // Only the accepted dirty extent is allowed to advance the file's EOF.
  const uint64_t end = start + record.pos + eof_len;

  const auto matches = [&](metadata::ChunkID chunk_id, bool *present) -> utils::Status {
    std::vector<SliceRecord> records;
    auto status = slice_metadata_->ReadRecords(chunk_id, &records);
    if (status.IsNotFound()) {
      *present = false;
      return utils::Status::OK();
    }
    if (!status.ok()) {
      return status;
    }
    *present = std::any_of(records.begin(), records.end(), [&](const SliceRecord &current) {
      return current.pos == record.pos && current.id == record.id && current.size == record.size &&
             current.off == record.off && current.len == record.len;
    });
    return utils::Status::OK();
  };

  // This loop only retries definitely rejected FileMetadata preconditions.
  // An ambiguous reply does not authorize a second non-idempotent append.
  for (int attempt = 0; attempt < 8; ++attempt) {
    metadata::FileChunkSnapshot snapshot;
    status = meta_->ReadFileChunkSnapshot(ino_, Index(), &snapshot);
    if (!status.ok()) {
      return status;
    }
    const metadata::FileSizePrecondition expected{.eof = snapshot.inode.attr.size, .boundary = snapshot.eof_boundary};
    const bool already_attached = snapshot.chunk_id.has_value();
    metadata::ChunkID chunk_id;
    if (already_attached) {
      chunk_id = *snapshot.chunk_id;
    } else {
      if (!candidate->created_chunk_id.has_value()) {
        status = slice_metadata_->AllocateChunkID(&chunk_id);
        if (!status.ok()) {
          return status;
        }
        if (chunk_id == metadata::kInvalidChunkID || chunk_id.Value() > metadata::kMaxChunkIDValue) {
          return utils::Status::Malformed("Slice allocator returned an invalid ChunkID");
        }
        candidate->created_chunk_id = chunk_id;
      }
      chunk_id = *candidate->created_chunk_id;
    }

    if (candidate->recorded_chunk_id != chunk_id) {
      if (candidate->uncertain_record_chunk_id == chunk_id) {
        bool present = false;
        status = matches(chunk_id, &present);
        if (!status.ok() || !present) {
          return utils::Status::OutcomeUnknown("Slice record append outcome is still ambiguous");
        }
        candidate->recorded_chunk_id = chunk_id;
        candidate->uncertain_record_chunk_id.reset();
      } else {
        status = already_attached ? slice_metadata_->AppendRecord(chunk_id, record)
                                  : slice_metadata_->CreateIndex(chunk_id, record);
        if (status.IsOutcomeUnknown()) {
          candidate->uncertain_record_chunk_id = chunk_id;
          bool present = false;
          bool confirmed = false;
          {
            const auto status = matches(chunk_id, &present);
            confirmed = status.ok() && present;
          }
          if (!confirmed) {
            return status;
          }
          status = utils::Status::OK();
        }
        if (!status.ok()) {
          return status;
        }
        candidate->recorded_chunk_id = chunk_id;
        candidate->uncertain_record_chunk_id.reset();
      }
    }

    status = already_attached ? meta_->FinalizeAttachedWrite(ino_, Index(), chunk_id, end, expected)
                              : meta_->AttachPrepared(ino_, Index(), chunk_id, end, expected);
    if (status.ok()) {
      return status;
    }
    if (status.IsOutcomeUnknown()) {
      // Initial attachment can be confirmed from a fresh authoritative
      // mapping/EOF snapshot; a lost mtime-only finalization reply cannot
      // be distinguished from a failed mutation, so remain conservative.
      if (!already_attached) {
        metadata::FileChunkSnapshot observed;
        bool confirmed = false;
        {
          const auto status = meta_->ReadFileChunkSnapshot(ino_, Index(), &observed);
          confirmed = status.ok() && observed.chunk_id == chunk_id && observed.inode.attr.size >= end;
        }
        if (confirmed) {
          return utils::Status::OK();
        }
      }
      return status;
    }
    if (status.ToErrno() != EEXIST) {
      return status;
    }
  }
  return utils::Status::AlreadyExists("Slice publication lost too many EOF/attachment races");
}

void SliceChunk::RebuildDirtyLocked() {
  dirty_.clear();
  for (const auto &candidate : frozen_) {
    if (candidate->visible_len > 0) {
      AcceptWrite(dirty_, candidate->pos, std::string_view(candidate->payload).substr(0, candidate->visible_len));
    }
  }
  for (const auto &[pos, bytes] : active_) {
    AcceptWrite(dirty_, pos, bytes);
  }
}

void SliceChunk::TruncateLocal(size_t size) {
  CaptureBoundary(size);
  std::lock_guard<utils::FiberRWMutex> lock(mutex_);
  ++truncate_epoch_;
  ClipRanges(dirty_, size);
  ClipRanges(active_, size);
  active_bytes_ = 0;
  for (const auto &[pos, bytes] : active_) {
    active_bytes_ += bytes.size();
  }
  for (auto &candidate : frozen_) {
    const size_t remaining = size > candidate->pos ? size - candidate->pos : 0;
    candidate->visible_len = std::min(candidate->visible_len, remaining);
    candidate->eof_len = std::min(candidate->eof_len, candidate->visible_len);
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
