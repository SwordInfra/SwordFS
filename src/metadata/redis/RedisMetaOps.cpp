// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaOps.hpp"

#include <folly/logging/xlog.h>
#include <sys/stat.h>

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

#include "chunk/IChunkOverwriteStrategy.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisDirIterator.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"
#include "utils/Logging.hpp"

namespace swordfs::metadata {
namespace {

// Decode a hash field that holds an inode id. Persisted queue state is
// durable data that can be corrupted or tampered with, so anything that is
// not exactly one inode id is reported as malformed rather than coerced.
utils::Status ParseInodeField(const std::string &field, std::string_view what, InodeID &ino) {
  uint64_t parsed = 0;
  const auto [end, error] = std::from_chars(field.data(), field.data() + field.size(), parsed);
  if (error != std::errc{} || end != field.data() + field.size() || parsed == 0) {
    return utils::Status::Malformed("invalid " + std::string(what) + " record: field is not an inode id");
  }
  ino = parsed;
  return utils::Status::OK();
}

}  // namespace

RedisMetaOps::RedisMetaOps(const RedisMetaConfig &config, std::string_view volume_name)
    : backend_(std::make_shared<RedisBackendContext>(
          config, static_cast<size_t>(std::max(1, swordfs::config::ConfigCenter::Instance().meta_thread_count())))),
      key_(config.db, volume_name) {
  utils::ExpectInThreadDomain();
}

RedisMetaOps::~RedisMetaOps() {
  utils::ExpectInThreadDomain();
  backend_->Shutdown();
}

utils::Status RedisMetaOps::BindChunkOverwriteStrategy(const chunk::IChunkOverwriteStrategy *strategy) {
  utils::ExpectInThreadDomain();
  if (strategy == nullptr) {
    return utils::Status::InvalidArgument("chunk strategy is null");
  }
  chunk_strategy_ = strategy;
  return utils::Status::OK();
}

utils::Status RedisMetaOps::Initialize() {
  utils::ExpectInThreadDomain();
  return backend_->executor().RunFromThread([this] { return backend_->client().Ping(); });
}

utils::Status RedisMetaOps::FormatVolume(const SwordFsVolume &config) {
  utils::ExpectInThreadDomain();

  SwordFsInode root;
  root.ino = kRootInodeId;
  root.parent_ino = kRootInodeId;
  root.attr = SwordFsAttr(kRootInodeId, S_IFDIR | 0755);
  std::string root_value;
  auto status = root.SerializeTo(&root_value);
  if (!status.ok()) {
    return status;
  }

  status = backend_->executor().RunFromThread([&] {
    return backend_->client().Transact([&](RedisKvTxn &txn) {
      std::string existing;
      auto txn_status = txn.Get(key_.Format(), &existing);
      if (txn_status.ok()) {
        return utils::Status::AlreadyExists("Redis metadata volume is already formatted");
      }
      if (!txn_status.IsNotFound()) {
        return txn_status;
      }
      txn_status = txn.Set(key_.Format(), config.SerializeTo());
      if (!txn_status.ok()) {
        return txn_status;
      }
      txn_status = txn.Set(key_.NextIno(), std::to_string(kRootInodeId));
      if (!txn_status.ok()) {
        return txn_status;
      }
      txn_status = txn.Set(key_.NextChunkRevision(), "0");
      if (!txn_status.ok()) {
        return txn_status;
      }
      txn_status = txn.Set(key_.InodeCount(), "1");
      if (!txn_status.ok()) {
        return txn_status;
      }
      return txn.Set(key_.Inode(kRootInodeId), root_value);
    });
  });
  if (status.ok()) {
    chunk_size_ = config.chunk_size;
  }
  return status;
}

utils::Status RedisMetaOps::LoadVolume(SwordFsVolume *config) {
  utils::ExpectInThreadDomain();
  if (config == nullptr) {
    return utils::Status::InvalidArgument("Redis volume config output is null");
  }

  std::string value;
  auto status = backend_->executor().RunFromThread([&] { return backend_->client().Get(key_.Format(), &value); });
  if (!status.ok()) {
    return status;
  }

  SwordFsVolume loaded;
  status = loaded.ParseFrom(value);
  if (!status.ok()) {
    return status;
  }
  if (loaded.name != config->name) {
    return utils::Status::Malformed("volume name does not match Redis metadata namespace");
  }

  chunk_size_ = loaded.chunk_size;
  *config = std::move(loaded);
  return utils::Status::OK();
}

utils::Status RedisMetaOps::CreateDirIterator(InodeID ino, std::vector<SwordFsEntry> prefix_entries,
                                              std::shared_ptr<DirIterator> *iterator) {
  utils::ExpectInFiberDomain();
  if (iterator == nullptr) {
    return utils::Status::InvalidArgument("directory iterator output is null");
  }
  *iterator = std::make_shared<RedisDirIterator>(backend_, key_.Directory(ino), std::move(prefix_entries));
  return utils::Status::OK();
}

utils::Status RedisMetaOps::GetInode(InodeID ino, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode output is null");
  }

  std::string value;
  auto status = backend_->executor().RunFromFiber([&] { return backend_->client().Get(key_.Inode(ino), &value); });
  if (!status.ok()) {
    return status;
  }
  return out->ParseFrom(value);
}

utils::Status RedisMetaOps::GetInodes(const std::vector<InodeID> &inode_ids,
                                      std::vector<std::optional<SwordFsInode>> *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("inode batch output is null");
  }
  if (inode_ids.empty()) {
    out->clear();
    return utils::Status::OK();
  }

  std::vector<std::string> keys;
  keys.reserve(inode_ids.size());
  for (const InodeID requested_ino : inode_ids) {
    keys.push_back(key_.Inode(requested_ino));
  }

  std::vector<std::optional<std::string>> values;
  auto status = backend_->executor().RunFromFiber([&] { return backend_->client().MGet(keys, &values); });
  if (!status.ok()) {
    return status;
  }

  std::vector<std::optional<SwordFsInode>> result;
  result.reserve(values.size());
  for (size_t i = 0; i < values.size(); ++i) {
    const auto &value = values[i];
    if (!value.has_value()) {
      result.emplace_back(std::nullopt);
      continue;
    }
    SwordFsInode inode;
    status = inode.ParseFrom(value.value());
    if (!status.ok()) {
      return status;
    }
    if (inode.ino != inode_ids[i]) {
      return utils::Status::Malformed("Redis inode batch record identity mismatch");
    }
    result.emplace_back(std::move(inode));
  }
  *out = std::move(result);
  return utils::Status::OK();
}

utils::Status RedisMetaOps::LookupEntry(InodeID parent_ino, std::string_view name, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("Lookup output is null");
  }

  SwordFsInode result;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) { return txn.LookupEntry(parent_ino, name, &result); });
  if (status.ok()) {
    *out = result;
  }
  return status;
}

utils::Status RedisMetaOps::SetAttr(InodeID ino, const SwordFsAttr &requested, SetAttrField fields, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  SwordFsInode result;
  std::vector<PendingDelete> detached_chunks;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) {
    return txn.SetAttr(ino, requested, fields, out != nullptr ? &result : nullptr, &detached_chunks);
  });
  if (status.ok() && out != nullptr) {
    *out = result;
  }
  if (status.ok()) {
    RegisterPendingDeletesBestEffort(ino, detached_chunks, "setattr shrink");
  }
  return status;
}

utils::Status RedisMetaOps::Truncate(InodeID ino, uint64_t size) {
  utils::ExpectInFiberDomain();
  std::vector<PendingDelete> detached_chunks;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) { return txn.Truncate(ino, size, &detached_chunks); });
  if (status.ok()) {
    RegisterPendingDeletesBestEffort(ino, detached_chunks, "truncate");
  }
  return status;
}

utils::Status RedisMetaOps::TouchInode(InodeID ino, SetAttrField fields) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.TouchInode(ino, fields); });
}

utils::Status RedisMetaOps::PrepareReclaim(InodeID ino, std::optional<ReclaimWork> *work) {
  utils::ExpectInFiberDomain();
  if (work == nullptr) {
    return utils::Status::InvalidArgument("reclaim work output is null");
  }
  work->reset();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.PrepareReclaim(ino, *work); });
}

utils::Status RedisMetaOps::CompleteReclaim(InodeID ino) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.CompleteReclaim(ino); });
}

utils::Status RedisMetaOps::VisitOrphanCandidates(const std::function<utils::Status(InodeID)> &visitor) {
  utils::ExpectInFiberDomain();
  if (!visitor) {
    return utils::Status::InvalidArgument("orphan candidate visitor is null");
  }

  // Snapshot before visiting: the visitor typically reclaims the visited
  // inode, which mutates the hash a cursor-based scan is walking.
  std::vector<InodeID> candidates;
  auto status = CollectOrphanCandidates(candidates);
  if (!status.ok()) {
    return status;
  }
  for (InodeID ino : candidates) {
    status = visitor(ino);
    if (!status.ok()) {
      return status;
    }
  }
  return utils::Status::OK();
}

utils::Status RedisMetaOps::VisitPendingReclaims(const std::function<utils::Status(const ReclaimWork &)> &visitor) {
  utils::ExpectInFiberDomain();
  if (!visitor) {
    return utils::Status::InvalidArgument("pending reclaim visitor is null");
  }

  std::vector<ReclaimWork> pending;
  auto status = CollectPendingReclaims(pending);
  if (!status.ok()) {
    return status;
  }
  for (const auto &work : pending) {
    status = visitor(work);
    if (!status.ok()) {
      return status;
    }
  }
  return utils::Status::OK();
}

utils::Status RedisMetaOps::VisitPendingDeletesBatch(size_t max_items,
                                                     const std::function<utils::Status(const PendingDelete &)> &visitor,
                                                     bool *has_more) {
  utils::ExpectInFiberDomain();
  if (max_items == 0) {
    return utils::Status::InvalidArgument("pending delete batch size must be positive");
  }
  if (!visitor) {
    return utils::Status::InvalidArgument("pending delete visitor is null");
  }
  if (has_more == nullptr) {
    return utils::Status::InvalidArgument("pending delete has-more output is null");
  }

  std::lock_guard<utils::FiberMutex> lock(pending_delete_scan_mutex_);
  *has_more = false;
  if (pending_delete_page_offset_ >= pending_delete_page_.size()) {
    std::vector<std::pair<std::string, std::string>> values;
    uint64_t next_cursor = 0;
    auto status = backend_->executor().RunFromFiber([&] {
      return backend_->client().HScan(key_.PendingDeletes(), pending_delete_cursor_, max_items, &values, &next_cursor);
    });
    if (!status.ok()) {
      return status;
    }

    std::vector<PendingDelete> page;
    page.reserve(values.size());
    for (auto &[object_key, encoded] : values) {
      PendingDelete pending;
      status = ParsePendingDelete(object_key, encoded, pending);
      if (!status.ok()) {
        return status;
      }
      page.push_back(std::move(pending));
    }

    pending_delete_page_ = std::move(page);
    pending_delete_page_offset_ = 0;
    pending_delete_cursor_ = next_cursor;
  }

  size_t visited = 0;
  while (pending_delete_page_offset_ < pending_delete_page_.size() && visited < max_items) {
    auto status = visitor(pending_delete_page_[pending_delete_page_offset_]);
    if (!status.ok()) {
      return status;
    }
    ++pending_delete_page_offset_;
    ++visited;
  }

  if (pending_delete_page_offset_ >= pending_delete_page_.size()) {
    pending_delete_page_.clear();
    pending_delete_page_offset_ = 0;
  }
  *has_more = !pending_delete_page_.empty() || pending_delete_cursor_ != 0;
  return utils::Status::OK();
}

utils::Status RedisMetaOps::CompletePendingDelete(std::string_view object_key) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.CompletePendingDelete(object_key); });
}

utils::Status RedisMetaOps::CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                                        const SwordFsChunk &replacement, const ChunkPublishIntent &intent) {
  utils::ExpectInFiberDomain();
  // Validate before opening the publication transaction. RedisMetaTxn repeats
  // these checks at the transaction boundary as defense in depth.
  if (!replacement.IsValidForChunkSize(chunk_size_)) {
    return utils::Status::InvalidArgument("replacement chunk descriptor is invalid");
  }
  if (expected.has_value() && !expected->IsValidForChunkSize(chunk_size_)) {
    return utils::Status::InvalidArgument("expected chunk descriptor is invalid");
  }
  if (expected.has_value() && replacement.revision <= expected->revision) {
    return utils::Status::InvalidArgument("replacement revision must increase");
  }
  if (expected.has_value() && expected->index != replacement.index) {
    return utils::Status::InvalidArgument("replacement must preserve chunk index");
  }

  utils::Status publication_result;
  std::optional<PendingDelete> cleanup_candidate;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) {
    return txn.CommitChunk(ino, expected, replacement, publication_result, cleanup_candidate, intent);
  });
  if (!status.ok()) {
    // A participant rejection is a known non-publication: its queued writes
    // were discarded. An EXEC error leaves publication_result OK because the
    // outcome can be partial or ambiguous and must not trigger deletion.
    if (!publication_result.ok() && cleanup_candidate.has_value()) {
      RegisterPendingDeletesBestEffort(ino, std::vector<PendingDelete>{*cleanup_candidate},
                                       "rejected chunk publication");
    }
    return status;
  }
  if (cleanup_candidate.has_value()) {
    RegisterPendingDeletesBestEffort(ino, std::vector<PendingDelete>{*cleanup_candidate}, "chunk publication");
  }
  return publication_result;
}

utils::Status RedisMetaOps::FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) {
  utils::ExpectInFiberDomain();
  if (chunk == nullptr) {
    return utils::Status::InvalidArgument("chunk output is null");
  }

  std::string value;
  auto status = backend_->executor().RunFromFiber(
      [&] { return backend_->client().HGet(key_.Chunk(ino), std::to_string(idx), &value); });
  if (!status.ok()) {
    return status;
  }
  status = chunk->ParseFrom(value);
  if (!status.ok()) {
    return status;
  }
  if (chunk->index != idx || !chunk->IsValidForChunkSize(chunk_size_)) {
    return utils::Status::Malformed("persisted chunk descriptor does not match its canonical identity");
  }
  return utils::Status::OK();
}

utils::Status RedisMetaOps::LoadChunkView(InodeID ino, ChunkIndex idx, ChunkView *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk view output is null");
  }

  ChunkView view;
  utils::Status read_result;
  // Even a semantic read error must reach the read-only EXEC, so WATCH can
  // reject a view assembled while its public or private hash changed.
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) {
    view = {};
    read_result = txn.LoadChunkView(ino, idx, &view);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }
  if (!read_result.ok()) {
    return read_result;
  }
  *out = std::move(view);
  return utils::Status::OK();
}

void RedisMetaOps::RegisterPendingDeletesBestEffort(InodeID ino, const std::vector<PendingDelete> &work,
                                                    std::string_view reason) {
  utils::ExpectInFiberDomain();
  if (work.empty()) {
    return;
  }
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) { return txn.RegisterPendingDeletes(work); });
  if (!status.ok()) {
    SWORDFS_LOG_WARN << "Best-effort pending-delete registration failed after " << reason << ": ino=" << ino
                     << " items=" << work.size() << " — " << status.message();
  }
}

utils::Status RedisMetaOps::CollectOrphanCandidates(std::vector<InodeID> &out) {
  utils::ExpectInFiberDomain();
  out.clear();

  constexpr size_t kScanBatchSize = 128;
  uint64_t cursor = 0;
  do {
    std::vector<std::pair<std::string, std::string>> values;
    uint64_t next_cursor = 0;
    auto status = backend_->executor().RunFromFiber(
        [&] { return backend_->client().HScan(key_.Orphans(), cursor, kScanBatchSize, &values, &next_cursor); });
    if (!status.ok()) {
      return status;
    }
    for (const auto &[field, value] : values) {
      (void)value;
      InodeID ino = 0;
      status = ParseInodeField(field, "orphan candidate", ino);
      if (!status.ok()) {
        return status;
      }
      out.push_back(ino);
    }
    cursor = next_cursor;
  } while (cursor != 0);

  std::sort(out.begin(), out.end());
  return utils::Status::OK();
}

utils::Status RedisMetaOps::CollectPendingReclaims(std::vector<ReclaimWork> &out) {
  utils::ExpectInFiberDomain();
  out.clear();

  constexpr size_t kScanBatchSize = 128;
  uint64_t cursor = 0;
  do {
    std::vector<std::pair<std::string, std::string>> values;
    uint64_t next_cursor = 0;
    auto status = backend_->executor().RunFromFiber(
        [&] { return backend_->client().HScan(key_.Reclaims(), cursor, kScanBatchSize, &values, &next_cursor); });
    if (!status.ok()) {
      return status;
    }
    for (const auto &[field, value] : values) {
      InodeID ino = 0;
      status = ParseInodeField(field, "pending reclaim", ino);
      if (!status.ok()) {
        return status;
      }
      ReclaimWork work;
      status = work.ParseFrom(value);
      if (!status.ok()) {
        return status;
      }
      if (work.ino != ino) {
        return utils::Status::Malformed("pending reclaim record inode mismatch");
      }
      out.push_back(std::move(work));
    }
    cursor = next_cursor;
  } while (cursor != 0);

  std::sort(out.begin(), out.end(), [](const ReclaimWork &a, const ReclaimWork &b) { return a.ino < b.ino; });
  return utils::Status::OK();
}

utils::Status RedisMetaOps::ParsePendingDelete(std::string_view object_key, std::string_view encoded,
                                               PendingDelete &out) const {
  PendingDelete pending;
  auto status = pending.ParseFrom(encoded);
  if (!status.ok()) {
    return status;
  }
  if (object_key != pending.id) {
    return utils::Status::Malformed("pending delete queue id mismatch");
  }
  out = std::move(pending);
  return utils::Status::OK();
}

utils::Status RedisMetaOps::AllocateInode(InodeID *ino) {
  utils::ExpectInFiberDomain();
  if (ino == nullptr) {
    return utils::Status::InvalidArgument("inode id output is null");
  }
  return backend_->executor().RunFromFiber([&] { return backend_->client().Incr(key_.NextIno(), ino); });
}

utils::Status RedisMetaOps::AllocateChunkRevision(ChunkRevision *revision) {
  utils::ExpectInFiberDomain();
  if (revision == nullptr) {
    return utils::Status::InvalidArgument("chunk revision output is null");
  }
  return backend_->executor().RunFromFiber([&] { return backend_->client().Incr(key_.NextChunkRevision(), revision); });
}

utils::Status RedisMetaOps::TransactFromFiber(const std::function<utils::Status(RedisMetaTxn &)> &callback) {
  utils::ExpectInFiberDomain();
  return backend_->executor().RunFromFiber([&] {
    return backend_->client().Transact([&](RedisKvTxn &kv_txn) {
      utils::ExpectInThreadDomain();
      RedisMetaTxn txn(kv_txn, key_, chunk_size_, chunk_strategy_);
      return callback(txn);
    });
  });
}

}  // namespace swordfs::metadata
