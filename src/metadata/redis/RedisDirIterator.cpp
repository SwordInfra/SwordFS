// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisDirIterator.hpp"

#include <glog/logging.h>

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "metadata/redis/RedisMetaClient.hpp"

namespace swordfs::metadata {
namespace {

constexpr size_t kHScanCount = 128;

class RedisDirEntryCache final {
 public:
  RedisDirEntryCache(std::shared_ptr<RedisMetaClient> client, std::string key)
      : client_(std::move(client)), key_(std::move(key)) {
    Reset();
  }

  Status Get(size_t index, SwordFsEntry *entry) {
    CHECK(entry != nullptr);
    auto status = EnsureLoaded(index);
    if (!status.ok()) {
      return status;
    }
    if (index >= base_index_ + entries_.size()) {
      CHECK(exhausted_);
      return Status::EndOfDirectory("directory end");
    }

    const auto &[name, value] = entries_[index - base_index_];
    status = entry->ParseFrom(value);
    if (!status.ok()) {
      return status;
    }
    if (entry->name != name) {
      return Status::Malformed("Redis directory field does not match serialized entry name");
    }
    return Status::OK();
  }

 private:
  void Reset() {
    entries_.clear();
    base_index_ = 0;
    cursor_ = 0;
    exhausted_ = false;
  }

  Status EnsureLoaded(size_t index) {
    // Only the HSCAN batch containing the requested index is retained. A
    // backward seek may therefore refer to an entry that has already been
    // discarded; restart the scan from directory index 0 in that case.
    if (index < base_index_) {
      Reset();
    }

    if (index < base_index_ + entries_.size()) {
      return Status::OK();
    }

    // The requested index is beyond the cached batch. Advance the logical base
    // to the next HSCAN batch and release the current batch before scanning.
    base_index_ += entries_.size();
    entries_.clear();

    while (!exhausted_) {
      std::vector<std::pair<std::string, std::string>> values;
      uint64_t next_cursor = cursor_;
      // cursor_ is Redis' opaque HSCAN continuation cursor. kHScanCount is only
      // a work/result-size hint; Redis may return more or fewer entries. A
      // returned cursor of 0 means the complete hash scan has finished.
      auto status = client_->HScan(key_, cursor_, kHScanCount, &values, &next_cursor);
      if (!status.ok()) {
        return status;
      }
      cursor_ = next_cursor;
      if (cursor_ == 0) {
        exhausted_ = true;
      }

      // This batch covers [base_index_, base_index_ + values.size()). Cache it
      // only when it contains the requested index; otherwise skip it entirely.
      if (index < base_index_ + values.size()) {
        entries_ = std::move(values);
        return Status::OK();
      }

      base_index_ += values.size();
    }
    return Status::OK();
  }

 private:
  std::shared_ptr<RedisMetaClient> client_;
  std::string key_;
  std::vector<std::pair<std::string, std::string>> entries_;
  size_t base_index_ = 0;
  uint64_t cursor_ = 0;
  bool exhausted_ = false;
};

}  // namespace

class RedisDirIterator::Impl {
 public:
  Impl(std::shared_ptr<RedisMetaClient> client, std::string key, std::vector<SwordFsEntry> prefix_entries)
      : cache_(std::move(client), std::move(key)), prefix_entries_(std::move(prefix_entries)) {
  }

  Status Seek(uint64_t cookie) {
    std::lock_guard lock(mutex_);
    position_ = cookie;
    pending_next_.reset();
    return Status::OK();
  }

  Status Peek(SwordFsEntry *entry, uint64_t *next_cookie) {
    if (entry == nullptr || next_cookie == nullptr) {
      return Status::InvalidArgument("directory iterator output is null");
    }
    std::lock_guard lock(mutex_);
    if (pending_next_) {
      return Status::InvalidArgument("directory iterator has pending entry");
    }

    if (position_ < prefix_entries_.size()) {
      *entry = prefix_entries_[static_cast<size_t>(position_)];
    } else {
      const size_t cache_index = static_cast<size_t>(position_ - prefix_entries_.size());
      auto status = cache_.Get(cache_index, entry);
      if (!status.ok()) {
        return status;
      }
    }

    *next_cookie = position_ + 1;
    pending_next_ = *next_cookie;
    return Status::OK();
  }

  void Advance() {
    std::lock_guard lock(mutex_);
    CHECK(pending_next_.has_value());
    position_ = *pending_next_;
    pending_next_.reset();
  }

 private:
  std::mutex mutex_;
  RedisDirEntryCache cache_;
  std::vector<SwordFsEntry> prefix_entries_;
  uint64_t position_ = 0;
  std::optional<uint64_t> pending_next_;
};

RedisDirIterator::RedisDirIterator(std::shared_ptr<RedisMetaClient> client, std::string key,
                                   std::vector<SwordFsEntry> prefix_entries)
    : impl_(std::make_unique<Impl>(std::move(client), std::move(key), std::move(prefix_entries))) {
}

RedisDirIterator::~RedisDirIterator() = default;

Status RedisDirIterator::Seek(uint64_t cookie) {
  return impl_->Seek(cookie);
}

Status RedisDirIterator::Peek(SwordFsEntry *entry, uint64_t *next_cookie) {
  return impl_->Peek(entry, next_cookie);
}

void RedisDirIterator::Advance() {
  impl_->Advance();
}

}  // namespace swordfs::metadata
