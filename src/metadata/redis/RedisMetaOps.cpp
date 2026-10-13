// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisMetaOps.hpp"

#include <folly/logging/xlog.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisCOWChunkMetadata.hpp"
#include "metadata/redis/RedisDirIterator.hpp"
#include "metadata/redis/RedisKvTxn.hpp"
#include "metadata/redis/RedisMetaClient.hpp"
#include "metadata/redis/RedisMetaTxn.hpp"
#include "metadata/redis/RedisReclaimCoordinator.hpp"
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
      key_(config.db, volume_name),
      reclaim_reconcile_attempts_(std::max(1, config.retry_attempts)) {
  utils::ExpectInThreadDomain();
}

RedisMetaOps::~RedisMetaOps() {
  utils::ExpectInThreadDomain();
  backend_->Shutdown();
}

utils::Status RedisMetaOps::OpenChunkMetadata(ChunkType chunk_type, ChunkMetadataPtr *out) {
  utils::ExpectInThreadDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("chunk metadata output is null");
  }
  if (chunk_type != ChunkType::kCow) {
    return utils::Status::NotSupported("chunk metadata type is not implemented: " +
                                       std::string(ChunkTypeName(chunk_type)));
  }
  if (chunk_metadata_ != nullptr) {
    *out = chunk_metadata_;
    return utils::Status::OK();
  }
  chunk_metadata_ = std::make_shared<RedisCOWChunkMetadata>(backend_, key_);
  *out = chunk_metadata_;
  return utils::Status::OK();
}

utils::Status RedisMetaOps::BindChunkCleanupWorkEncoder(chunk::internal::ChunkCleanupWorkEncoder *encoder) {
  utils::ExpectInThreadDomain();
  if (encoder == nullptr) {
    return utils::Status::InvalidArgument("cleanup work encoder is null");
  }
  cleanup_work_encoder_ = encoder;
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
      // The counter must never be bootstrapped at mount: formatting defines
      // its history in the same atomic commit as the volume identity. Keep
      // all initialization keys together so every SET has the same error
      // handling and none can be omitted from the atomic format transaction.
      const std::array<std::pair<std::string, std::string>, 5> initial_keys{{
          {key_.Format(), config.SerializeTo()},
          {key_.Inode(kRootInodeId), root_value},
          {key_.NextIno(), std::to_string(kRootInodeId)},
          {key_.InodeCount(), "1"},
          {key_.OwnerGeneration(), "0"},
      }};
      for (const auto &[key, value] : initial_keys) {
        auto queued = txn.Set(key, value);
        if (!queued.ok()) {
          return queued;
        }
      }
      return utils::Status::OK();
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
  if (HasSetAttrField(fields, SetAttrField::kSize)) {
    return utils::Status::InvalidArgument("size changes require FileReadWriter");
  }
  auto status = TransactFromFiber(
      [&](RedisMetaTxn &txn) { return txn.SetAttr(ino, requested, fields, out != nullptr ? &result : nullptr); });
  if (status.ok() && out != nullptr) {
    *out = result;
  }
  return status;
}

utils::Status RedisMetaOps::SetInodeFlags(InodeID ino, InodeFlag inode_flags, SwordFsInode *out) {
  utils::ExpectInFiberDomain();
  SwordFsInode result;
  auto status = TransactFromFiber(
      [&](RedisMetaTxn &txn) { return txn.SetInodeFlags(ino, inode_flags, out != nullptr ? &result : nullptr); });
  if (status.ok() && out != nullptr) {
    *out = result;
  }
  return status;
}

utils::Status RedisMetaOps::SetXAttr(InodeID ino, std::string_view name, std::string_view value, XAttrSetMode mode) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.SetXAttr(ino, name, value, mode); });
}

utils::Status RedisMetaOps::GetXAttr(InodeID ino, std::string_view name, std::string *value) {
  utils::ExpectInFiberDomain();
  SwordFsInode inode;
  auto status = GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  return inode.GetXAttr(name, value);
}

utils::Status RedisMetaOps::ListXAttrs(InodeID ino, std::vector<std::string> *names) {
  utils::ExpectInFiberDomain();
  SwordFsInode inode;
  auto status = GetInode(ino, &inode);
  if (!status.ok()) {
    return status;
  }
  return inode.ListXAttrs(names);
}

utils::Status RedisMetaOps::RemoveXAttr(InodeID ino, std::string_view name) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.RemoveXAttr(ino, name); });
}

utils::Status RedisMetaOps::TouchInode(InodeID ino, SetAttrField fields) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.TouchInode(ino, fields); });
}

utils::Status RedisMetaOps::PrepareReclaimAttempt(InodeID ino) {
  std::optional<ReclaimWork> work;
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.PrepareReclaim(ino, work); });
}

utils::Status RedisMetaOps::PrepareReclaim(InodeID ino) {
  utils::ExpectInFiberDomain();
  return redis::internal::RunReclaimWithOutcomeReconciliation(
      reclaim_reconcile_attempts_, [&] { return PrepareReclaimAttempt(ino); },
      [&](SwordFsInode *inode) { return GetInode(ino, inode); });
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

  // Keep a resumable HSCAN page across visitor aborts. Reclaiming a visited
  // inode mutates the hash, so the cursor/page are owned by this scan rather
  // than reconstructed from zero after every bounded worker pass.
  constexpr size_t kScanBatchSize = 128;
  std::lock_guard<utils::FiberMutex> lock(orphan_scan_mutex_);
  for (;;) {
    if (orphan_scan_page_offset_ >= orphan_scan_page_.size()) {
      orphan_scan_page_.clear();
      orphan_scan_page_offset_ = 0;

      std::vector<std::pair<std::string, std::string>> values;
      uint64_t next_cursor = 0;
      auto status = backend_->executor().RunFromFiber([&] {
        return backend_->client().HScan(key_.Orphans(), orphan_scan_cursor_, kScanBatchSize, &values, &next_cursor);
      });
      if (!status.ok()) {
        return status;
      }

      std::vector<InodeID> page;
      page.reserve(values.size());
      for (const auto &[field, value] : values) {
        (void)value;
        InodeID ino = 0;
        status = ParseInodeField(field, "orphan candidate", ino);
        if (!status.ok()) {
          return status;
        }
        page.push_back(ino);
      }
      std::sort(page.begin(), page.end());
      orphan_scan_page_ = std::move(page);
      orphan_scan_cursor_ = next_cursor;

      if (orphan_scan_page_.empty() && orphan_scan_cursor_ == 0) {
        return utils::Status::OK();
      }
    }

    while (orphan_scan_page_offset_ < orphan_scan_page_.size()) {
      auto status = visitor(orphan_scan_page_[orphan_scan_page_offset_]);
      if (!status.ok()) {
        return status;
      }
      ++orphan_scan_page_offset_;
    }

    if (orphan_scan_cursor_ == 0) {
      orphan_scan_page_.clear();
      orphan_scan_page_offset_ = 0;
      return utils::Status::OK();
    }
  }
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

utils::Status RedisMetaOps::RegisterPendingDelete(const PendingDelete &work) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.RegisterPendingDeletes({work}); });
}

utils::Status RedisMetaOps::CompletePendingDelete(std::string_view object_key) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.CompletePendingDelete(object_key); });
}

utils::Status RedisMetaOps::ReadFileChunkSnapshot(InodeID ino, ChunkIndex index, FileChunkSnapshot *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("file chunk snapshot output is null");
  }
  FileChunkSnapshot result;
  utils::Status read_status;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) {
    read_status = txn.ReadFileChunkSnapshot(ino, index, &result);
    return utils::Status::OK();  // Read-only EXEC validates all WATCHed keys.
  });
  if (!status.ok()) {
    return status;
  }
  if (read_status.ok()) {
    *out = std::move(result);
  }
  return read_status;
}

utils::Status RedisMetaOps::ReadFileMappingSnapshot(InodeID ino, FileMappingSnapshot *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("file mapping snapshot output is null");
  }
  FileMappingSnapshot result;
  utils::Status read_status;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) {
    read_status = txn.ReadFileMappingSnapshot(ino, &result);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }
  if (read_status.ok()) {
    *out = std::move(result);
  }
  return read_status;
}

utils::Status RedisMetaOps::ProbeAttachment(InodeID ino, ChunkIndex index, std::optional<ChunkID> *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("attachment output is null");
  }
  std::optional<ChunkID> result;
  utils::Status read_status;
  auto status = TransactFromFiber([&](RedisMetaTxn &txn) {
    read_status = txn.ProbeAttachment(ino, index, &result);
    return utils::Status::OK();
  });
  if (!status.ok()) {
    return status;
  }
  if (read_status.ok()) {
    *out = result;
  }
  return read_status;
}

utils::Status RedisMetaOps::AttachPrepared(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                                           const FileSizePrecondition &expected) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber([&](RedisMetaTxn &txn) { return txn.AttachPrepared(ino, index, chunk_id, end, expected); });
}

utils::Status RedisMetaOps::FinalizeAttachedWrite(InodeID ino, ChunkIndex index, ChunkID chunk_id, uint64_t end,
                                                  const FileSizePrecondition &expected) {
  utils::ExpectInFiberDomain();
  return TransactFromFiber(
      [&](RedisMetaTxn &txn) { return txn.FinalizeAttachedWrite(ino, index, chunk_id, end, expected); });
}

utils::Status RedisMetaOps::CommitShrink(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                         SetAttrField fields, ChunkSizeCommitResult *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("typed shrink result is null");
  }
  ChunkSizeCommitResult committed;
  auto status =
      TransactFromFiber([&](RedisMetaTxn &txn) { return txn.CommitShrink(ino, plan, requested, fields, &committed); });
  if (status.ok()) {
    *out = std::move(committed);
  }
  return status;
}

utils::Status RedisMetaOps::CommitGrow(InodeID ino, const ChunkSizePlan &plan, const SwordFsAttr &requested,
                                       SetAttrField fields, ChunkSizeCommitResult *out) {
  utils::ExpectInFiberDomain();
  if (out == nullptr) {
    return utils::Status::InvalidArgument("typed grow result is null");
  }
  ChunkSizeCommitResult committed;
  auto status =
      TransactFromFiber([&](RedisMetaTxn &txn) { return txn.CommitGrow(ino, plan, requested, fields, &committed); });
  if (status.ok()) {
    *out = std::move(committed);
  }
  return status;
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

utils::Status RedisMetaOps::TransactFromFiber(const std::function<utils::Status(RedisMetaTxn &)> &callback) {
  utils::ExpectInFiberDomain();
  return backend_->executor().RunFromFiber([&] {
    return backend_->client().Transact([&](RedisKvTxn &kv_txn) {
      utils::ExpectInThreadDomain();
      RedisMetaTxn txn(kv_txn, key_, chunk_size_, cleanup_work_encoder_);
      return callback(txn);
    });
  });
}

}  // namespace swordfs::metadata
