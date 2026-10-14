// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "chunk/slice/SliceRecord.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace swordfs::chunk::slice {
namespace {

utils::Status ValidateRecord(const SliceRecord &record, uint32_t chunk_size) {
  if (record.len == 0 || record.pos >= chunk_size || record.len > chunk_size - record.pos) {
    return utils::Status::InvalidArgument("Slice record exceeds logical chunk bounds or is empty");
  }
  if (record.id == SliceID()) {
    if (record.size != 0 || record.off != 0) {
      return utils::Status::InvalidArgument("Zero-cover record has nonzero object fields");
    }
    return utils::Status::OK();
  }
  if (record.id.Value() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) || record.size == 0 ||
      record.off > record.size || record.len > record.size - record.off) {
    return utils::Status::InvalidArgument("Slice object range or identity is invalid");
  }
  return utils::Status::OK();
}

void AppendCoalesced(const ResolvedSlice &span, std::vector<ResolvedSlice> *out) {
  if (!out->empty()) {
    auto &last = out->back();
    if (last.pos + last.len == span.pos && last.id == span.id &&
        (span.id == SliceID() || last.off + last.len == span.off)) {
      last.len += span.len;
      return;
    }
  }
  out->push_back(span);
}

}  // namespace

utils::Status ResolveRange(std::span<const SliceRecord> records, uint32_t chunk_size, uint32_t pos, uint32_t len,
                           std::vector<ResolvedSlice> *out) {
  if (out == nullptr || chunk_size == 0 || pos > chunk_size || len > chunk_size - pos) {
    return utils::Status::InvalidArgument("Slice read exceeds logical chunk bounds or output is null");
  }
  for (const auto &record : records) {
    auto status = ValidateRecord(record, chunk_size);
    if (!status.ok()) {
      return status;
    }
  }

  std::vector<ResolvedSlice> resolved;
  if (len == 0) {
    *out = std::move(resolved);
    return utils::Status::OK();
  }

  // Process committed records from newest to oldest. Each successful overlay
  // consumes previously uncovered pieces, so older records cannot reappear.
  // Ordered holes avoid rebuilding/sorting the full chunk for each record.
  std::map<uint32_t, uint32_t> uncovered = {{pos, pos + len}};
  for (auto record = records.rbegin(); record != records.rend() && !uncovered.empty(); ++record) {
    const uint32_t record_end = record->pos + record->len;
    auto gap = uncovered.upper_bound(record->pos);
    if (gap != uncovered.begin() && std::prev(gap)->second > record->pos) {
      --gap;
    }
    while (gap != uncovered.end() && gap->first < record_end) {
      const uint32_t cover_start = std::max(record->pos, gap->first);
      const uint32_t cover_end = std::min(record_end, gap->second);
      // Every reached gap intersects the record: the first gap is selected
      // only if it extends beyond record.pos; later gaps start before its end.
      const uint32_t gap_start = gap->first;
      const uint32_t gap_end = gap->second;
      gap = uncovered.erase(gap);
      if (gap_start < cover_start) {
        uncovered.emplace(gap_start, cover_start);
      }
      if (cover_end < gap_end) {
        uncovered.emplace(cover_end, gap_end);
      }
      resolved.push_back({cover_start, record->id,
                          record->id == SliceID() ? 0U : record->off + (cover_start - record->pos),
                          cover_end - cover_start});
      gap = uncovered.lower_bound(cover_end);
    }
  }
  for (const auto &[start, end] : uncovered) {
    resolved.push_back({start, SliceID(), 0, end - start});
  }

  // Presentation/read I/O order is logical address order, not write order.
  std::sort(resolved.begin(), resolved.end(),
            [](const ResolvedSlice &a, const ResolvedSlice &b) { return a.pos < b.pos; });
  std::vector<ResolvedSlice> merged;
  merged.reserve(resolved.size());
  for (const auto &span : resolved) {
    AppendCoalesced(span, &merged);
  }
  *out = std::move(merged);
  return utils::Status::OK();
}

}  // namespace swordfs::chunk::slice
