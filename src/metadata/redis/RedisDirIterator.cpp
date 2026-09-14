// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisDirIterator.hpp"

#include <glog/logging.h>

#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/Synchronization.hpp"

namespace swordfs::metadata {
namespace {

constexpr size_t kHScanCount = 128;

class RedisDirEntryCache final {
 public:
  RedisDirEntryCache(std::shared_ptr<RedisBackendContext> backend, std::string key)
      : backend_(std::move(backend)), key_(std::move(key)) {
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
    if (index < base_index_) {
      Reset();
    }
    if (index < base_index_ + entries_.size()) {
      return Status::OK();
    }

    base_index_ += entries_.size();
    entries_.clear();

    while (!exhausted_) {
      std::vector<std::pair<std::string, std::string>> values;
      uint64_t next_cursor = cursor_;
      auto status = backend_->executor().RunFromFiber(
          [&] { return backend_->client().HScan(key_, cursor_, kHScanCount, &values, &next_cursor); });
      if (!status.ok()) {
        return status;
      }
      cursor_ = next_cursor;
      if (cursor_ == 0) {
        exhausted_ = true;
      }
      if (index < base_index_ + values.size()) {
        entries_ = std::move(values);
        return Status::OK();
      }
      base_index_ += values.size();
    }
    return Status::OK();
  }

  std::shared_ptr<RedisBackendContext> backend_;
  std::string key_;
  std::vector<std::pair<std::string, std::string>> entries_;
  size_t base_index_ = 0;
  uint64_t cursor_ = 0;
  bool exhausted_ = false;
};

}  // namespace

class RedisDirIterator::Impl {
 public:
  Impl(std::shared_ptr<RedisBackendContext> backend, std::string key, std::vector<SwordFsEntry> prefix_entries)
      : cache_(std::move(backend), std::move(key)), prefix_entries_(std::move(prefix_entries)) {
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

  utils::FiberMutex mutex_;
  RedisDirEntryCache cache_;
  std::vector<SwordFsEntry> prefix_entries_;
  uint64_t position_ = 0;
  std::optional<uint64_t> pending_next_;
};

RedisDirIterator::RedisDirIterator(std::shared_ptr<RedisBackendContext> backend, std::string key,
                                   std::vector<SwordFsEntry> prefix_entries)
    : impl_(std::make_unique<Impl>(std::move(backend), std::move(key), std::move(prefix_entries))) {
}

RedisDirIterator::~RedisDirIterator() = default;

Status RedisDirIterator::Seek(uint64_t cookie) {
  utils::ExpectInFiberDomain();
  return impl_->Seek(cookie);
}

Status RedisDirIterator::Peek(SwordFsEntry *entry, uint64_t *next_cookie) {
  utils::ExpectInFiberDomain();
  return impl_->Peek(entry, next_cookie);
}

void RedisDirIterator::Advance() {
  utils::ExpectInFiberDomain();
  impl_->Advance();
}

}  // namespace swordfs::metadata
