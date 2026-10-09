// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileReadWriter — read, write, flush, and
// cross-file-handle sharing behaviour.

#include <folly/ScopeGuard.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "FiberTest.hpp"
#include "TestWatchdog.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/Chunk.hpp"
#include "chunk/cow/COWChunk.hpp"
#include "chunk/cow/COWCleanup.hpp"
#include "chunk/cow/COWObjectKey.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "metadata/mem/MemCOWChunkMetadata.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/FileHandle.hpp"
#include "vfs/FileReadWriter.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/VfsImpl.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkIndex;
using swordfs::metadata::IMetaEngine;
using swordfs::metadata::InodeID;
using swordfs::metadata::Limits;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SetAttrField;
using swordfs::metadata::SwordFsAttr;
using swordfs::metadata::SwordFsChunk;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsStatFs;
using swordfs::metadata::SwordFsVolume;
using swordfs::storage::IDataEngine;
using swordfs::test::RunInTestFiber;
using swordfs::utils::Status;
using swordfs::vfs::FileReadWriter;
using swordfs::vfs::InodeHandle;

constexpr size_t kMaxParallelFlushes = 2;
constexpr uint64_t kTestChunkSize = 1024;

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

static std::string Repeat(char c, size_t n) {
  return std::string(n, c);
}

static auto Buf(const std::string &s) {
  return *folly::IOBuf::copyBuffer(s.data(), s.size());
}

// Keep failure injection at the actual typed mechanism boundary; the retired
// IMetaEngine::AllocateChunkRevision cannot affect COW publication anymore.
class FaultingCOWChunkMetadata final : public swordfs::metadata::cow::COWChunkMetadata {
 public:
  Status AllocateChunkID(swordfs::metadata::ChunkID *out) override {
    return implementation_.AllocateChunkID(out);
  }
  Status AllocateRevision(swordfs::metadata::ChunkID id, swordfs::metadata::cow::COWChunkRevision *out) override {
    ++allocate_revision_calls;
    if (!allocate_revision_status.ok()) {
      return allocate_revision_status;
    }
    return implementation_.AllocateRevision(id, out);
  }
  Status GetHead(swordfs::metadata::ChunkID id, swordfs::metadata::cow::COWChunkHead *out) override {
    return implementation_.GetHead(id, out);
  }
  Status CompareExchangeHead(swordfs::metadata::ChunkID id,
                             const std::optional<swordfs::metadata::cow::COWChunkHead> &expected,
                             const swordfs::metadata::cow::COWChunkHead &replacement) override {
    return implementation_.CompareExchangeHead(id, expected, replacement);
  }
  Status EraseHead(swordfs::metadata::ChunkID id, const swordfs::metadata::cow::COWChunkHead &expected) override {
    return implementation_.EraseHead(id, expected);
  }

  int allocate_revision_calls = 0;
  Status allocate_revision_status = Status::OK();

 private:
  swordfs::metadata::MemCOWChunkMetadata implementation_;
};

// ────────────────────────────────────────────────────────────────
// MockDataEngine — in-memory IDataEngine
// ────────────────────────────────────────────────────────────────

class MockDataEngine : public IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override {
    ++put_calls;
    if (put_started_ != nullptr) {
      auto *started = put_started_;
      auto *release = put_release_;
      put_started_ = nullptr;
      put_release_ = nullptr;
      started->post();
      release->wait();
    } else if (concurrent_put_started_ != nullptr) {
      auto *started = concurrent_put_started_;
      concurrent_put_started_ = nullptr;
      started->post();
    }
    const bool matches_key = fail_put_key.empty() || key == fail_put_key;
    const bool matches_prefix = fail_put_prefix.empty() || key.starts_with(fail_put_prefix);
    if (remaining_forced_put_failures > 0) {
      --remaining_forced_put_failures;
      return put_status;
    }
    if (!put_status.ok() && matches_key && matches_prefix) {
      return put_status;
    }
    store_[std::string(key)] = std::string(reinterpret_cast<const char *>(data->data()), data->length());
    return Status::OK();
  }

  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) override {
    if (get_started_ != nullptr) {
      auto *started = get_started_;
      auto *release = get_release_;
      get_started_ = nullptr;
      get_release_ = nullptr;
      started->post();
      release->wait();
    } else if (concurrent_get_started_ != nullptr) {
      auto *started = concurrent_get_started_;
      concurrent_get_started_ = nullptr;
      started->post();
    }

    if (!get_status.ok()) {
      if (!get_error_payload.empty()) {
        if (out->tailroom() < get_error_payload.size()) {
          return Status::InvalidArgument("injected error payload exceeds output buffer");
        }
        std::memcpy(out->writableTail(), get_error_payload.data(), get_error_payload.size());
        out->append(get_error_payload.size());
      }
      return get_status;
    }

    auto it = store_.find(std::string(key));
    if (it == store_.end()) {
      return Status::NotFound("chunk not found");
    }
    const std::string &chunk = it->second;
    if (offset >= chunk.size()) {
      return Status::OK();
    }
    size_t len = (size == 0) ? chunk.size() - offset : std::min(size, chunk.size() - offset);
    std::memcpy(out->writableTail(), chunk.data() + offset, len);
    out->append(len);
    return Status::OK();
  }

  Status Delete(std::string_view key) override {
    delete_calls.push_back(std::string(key));
    store_.erase(std::string(key));
    return delete_status;
  }

  // Public for tests: the chunks the data engine knows about. Useful
  // for verifying that Truncate's data-engine Deletes match the chunks
  // the metadata engine removed from its chunk map.
  std::vector<std::string> StoredKeys() const {
    std::vector<std::string> out;
    out.reserve(store_.size());
    for (const auto &[k, _] : store_) {
      out.push_back(k);
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  void BlockNextGet(folly::fibers::Baton *started, folly::fibers::Baton *release,
                    folly::fibers::Baton *concurrent_get_started = nullptr) {
    get_started_ = started;
    get_release_ = release;
    concurrent_get_started_ = concurrent_get_started;
  }

  void BlockNextPut(folly::fibers::Baton *started, folly::fibers::Baton *release,
                    folly::fibers::Baton *concurrent_put_started = nullptr) {
    put_started_ = started;
    put_release_ = release;
    concurrent_put_started_ = concurrent_put_started;
  }

  std::vector<std::string> delete_calls;
  Status delete_status = Status::OK();
  Status get_status = Status::OK();
  std::string get_error_payload;
  Status put_status = Status::OK();
  std::string fail_put_key;
  std::string fail_put_prefix;
  int put_calls = 0;
  int remaining_forced_put_failures = 0;

 private:
  std::unordered_map<std::string, std::string> store_;
  folly::fibers::Baton *get_started_{nullptr};
  folly::fibers::Baton *get_release_{nullptr};
  folly::fibers::Baton *concurrent_get_started_{nullptr};
  folly::fibers::Baton *put_started_{nullptr};
  folly::fibers::Baton *put_release_{nullptr};
  folly::fibers::Baton *concurrent_put_started_{nullptr};
};

// ────────────────────────────────────────────────────────────────
// MockMetaEngine — minimal IMetaEngine with chunk support
// ────────────────────────────────────────────────────────────────

class MockMetaEngine : public IMetaEngine {
 public:
  Status OpenChunkMetadata(swordfs::metadata::ChunkType type, swordfs::metadata::ChunkMetadataPtr *out) override {
    if (out == nullptr || type != swordfs::metadata::ChunkType::kCow) {
      return Status::InvalidArgument("invalid mock COW metadata request");
    }
    *out = cow_metadata_;
    return Status::OK();
  }

  std::shared_ptr<FaultingCOWChunkMetadata> CowMetadata() const {
    return cow_metadata_;
  }

  Status ReadFileChunkSnapshot(InodeID ino, ChunkIndex index, swordfs::metadata::FileChunkSnapshot *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("missing file chunk snapshot output");
    }
    if (!find_chunk_status.ok() && (!find_chunk_error_idx.has_value() || *find_chunk_error_idx == index)) {
      return find_chunk_status;
    }
    swordfs::metadata::FileChunkSnapshot snapshot;
    auto status = GetInode(ino, &snapshot.inode);
    if (!status.ok()) {
      return status;
    }
    const auto &refs = typed_chunks_[ino];
    if (auto it = refs.find(index); it != refs.end()) {
      snapshot.chunk_id = it->second;
    }
    swordfs::metadata::ChunkSizeLayout layout;
    status = swordfs::metadata::PlanChunkSizeLayout(snapshot.inode.attr.size, kTestChunkSize, &layout);
    if (!status.ok()) {
      return status;
    }
    if (layout.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> boundary_id;
      if (auto it = refs.find(layout.boundary->index); it != refs.end()) {
        boundary_id = it->second;
      }
      snapshot.eof_boundary = swordfs::metadata::ChunkBoundarySnapshot{
          .index = layout.boundary->index, .chunk_id = boundary_id, .visible_prefix = layout.boundary->visible_prefix};
    }
    *out = std::move(snapshot);
    return Status::OK();
  }

  Status ReadFileMappingSnapshot(InodeID ino, swordfs::metadata::FileMappingSnapshot *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("missing file mapping snapshot output");
    }
    swordfs::metadata::FileMappingSnapshot snapshot;
    auto status = GetInode(ino, &snapshot.inode);
    if (!status.ok()) {
      return status;
    }
    for (const auto &[index, id] : typed_chunks_[ino]) {
      snapshot.mappings.push_back({.index = index, .chunk_id = id});
    }
    *out = std::move(snapshot);
    return Status::OK();
  }

  Status ProbeAttachment(InodeID ino, ChunkIndex index, std::optional<swordfs::metadata::ChunkID> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("missing attachment output");
    }
    out->reset();
    const auto &refs = typed_chunks_[ino];
    if (auto it = refs.find(index); it != refs.end()) {
      *out = it->second;
    }
    return Status::OK();
  }

  Status CheckFileState(InodeID ino, const swordfs::metadata::FileSizePrecondition &expected) {
    auto status = swordfs::metadata::ValidateFileSizePrecondition(expected, kTestChunkSize);
    if (!status.ok()) {
      return status;
    }
    if (expected.eof != static_cast<uint64_t>(file_size_)) {
      return Status::AlreadyExists("observed EOF changed");
    }
    if (expected.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> actual;
      status = ProbeAttachment(ino, expected.boundary->index, &actual);
      if (!status.ok()) {
        return status;
      }
      if (actual != expected.boundary->chunk_id) {
        return Status::AlreadyExists("observed EOF boundary changed");
      }
    }
    return Status::OK();
  }

  Status AttachPrepared(InodeID ino, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                        const swordfs::metadata::FileSizePrecondition &expected) override {
    PauseNextPublicationIfRequested();
    if (!attach_prepared_status.ok() && !attach_prepared_commit_on_error) {
      return attach_prepared_status;
    }
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, kTestChunkSize);
    if (!status.ok()) {
      return status;
    }
    status = CheckFileState(ino, expected);
    if (!status.ok()) {
      return status;
    }
    auto &refs = typed_chunks_[ino];
    if (!refs.emplace(index, id).second) {
      return Status::AlreadyExists("first attachment already exists");
    }
    file_size_ = std::max<uint64_t>(file_size_, end);
    return attach_prepared_status;
  }

  Status FinalizeAttachedWrite(InodeID ino, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                               const swordfs::metadata::FileSizePrecondition &expected) override {
    PauseNextPublicationIfRequested();
    if (!finalize_attached_status.ok() && !finalize_attached_commit_on_error) {
      return finalize_attached_status;
    }
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, kTestChunkSize);
    if (!status.ok()) {
      return status;
    }
    status = CheckFileState(ino, expected);
    if (!status.ok()) {
      return status;
    }
    auto &refs = typed_chunks_[ino];
    const auto it = refs.find(index);
    if (it == refs.end() || it->second != id) {
      return Status::AlreadyExists("attached ChunkID changed");
    }
    file_size_ = std::max<uint64_t>(file_size_, end);
    return finalize_attached_status;
  }

  Status CommitShrink(InodeID ino, const swordfs::metadata::ChunkSizePlan &plan, const SwordFsAttr &attr,
                      SetAttrField fields, swordfs::metadata::ChunkSizeCommitResult *out) override {
    return CommitSize(ino, plan, attr, fields, /*shrink=*/true, out);
  }
  Status CommitGrow(InodeID ino, const swordfs::metadata::ChunkSizePlan &plan, const SwordFsAttr &attr,
                    SetAttrField fields, swordfs::metadata::ChunkSizeCommitResult *out) override {
    return CommitSize(ino, plan, attr, fields, /*shrink=*/false, out);
  }

  Status CommitSize(InodeID ino, const swordfs::metadata::ChunkSizePlan &plan, const SwordFsAttr &attr,
                    SetAttrField fields, bool shrink, swordfs::metadata::ChunkSizeCommitResult *out) {
    if (!out || !HasSetAttrField(fields, SetAttrField::kSize) || plan.target_eof != attr.size) {
      return Status::InvalidArgument("invalid typed file size commit request");
    }
    swordfs::metadata::FileMappingSnapshot snapshot;
    auto status = ReadFileMappingSnapshot(ino, &snapshot);
    if (!status.ok()) {
      return status;
    }
    swordfs::metadata::ChunkSizePlan current;
    status = swordfs::metadata::ValidateSizeCommitPlan(plan, snapshot.inode.attr.size, kTestChunkSize,
                                                       snapshot.mappings, shrink, &current);
    if (!status.ok()) {
      return status;
    }
    if (!truncate_status_.ok() && !truncate_commit_on_error) {
      return truncate_status_;
    }
    if (!set_attr_status.ok() && !set_attr_commit_on_error) {
      return set_attr_status;
    }
    auto &refs = typed_chunks_[ino];
    out->detached.clear();
    swordfs::metadata::ChunkSizeLayout layout;
    status = swordfs::metadata::PlanChunkSizeLayout(attr.size, kTestChunkSize, &layout);
    if (!status.ok()) {
      return status;
    }
    if (shrink) {
      for (auto it = refs.begin(); it != refs.end();) {
        if (layout.ShouldDetach(it->first)) {
          out->detached.push_back({.index = it->first, .chunk_id = it->second});
          it = refs.erase(it);
        } else {
          ++it;
        }
      }
    }
    file_size_ = attr.size;
    out->inode = snapshot.inode;
    out->inode.attr.size = attr.size;
    out->boundary = current.boundary;
    if (!truncate_status_.ok() && truncate_commit_on_error) {
      return truncate_status_;
    }
    if (!set_attr_status.ok() && set_attr_commit_on_error) {
      return set_attr_status;
    }
    return Status::OK();
  }
  Status Initialize() override {
    return Status::OK();
  }
  Status FormatVolume(const SwordFsVolume &) override {
    return Status::OK();
  }
  Status LoadVolume(SwordFsVolume *) override {
    return Status::OK();
  }
  Limits GetLimits() const override {
    return {};
  }
  Status Lookup(InodeID, std::string_view, SwordFsInode *out) override {
    if (out) {
      *out = {};
    }
    return Status::OK();
  }
  Status GetInode(InodeID ino, SwordFsInode *out) override {
    if (!get_inode_status.ok()) {
      return get_inode_status;
    }
    const uint64_t snapshot_size = reported_file_size_.value_or(static_cast<uint64_t>(file_size_));
    if (get_inode_started_ != nullptr) {
      auto *started = get_inode_started_;
      auto *release = get_inode_release_;
      get_inode_started_ = nullptr;
      get_inode_release_ = nullptr;
      started->post();
      release->wait();
    }
    if (out) {
      *out = SwordFsInode(ino, SwordFsAttr(ino, S_IFREG | 0644), /*parent_ino=*/1);
      out->attr.size = snapshot_size;
    }
    return Status::OK();
  }
  Status GetInodes(const std::vector<InodeID> &inode_ids, std::vector<std::optional<SwordFsInode>> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("inode batch output is null");
    }
    out->clear();
    for (const InodeID requested_ino : inode_ids) {
      SwordFsInode inode;
      auto status = GetInode(requested_ino, &inode);
      if (!status.ok()) {
        return status;
      }
      out->emplace_back(std::move(inode));
    }
    return Status::OK();
  }
  Status Create(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkNod(InodeID, std::string_view, uint32_t, uint64_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status MkDir(InodeID, std::string_view, uint32_t, SwordFsInode *) override {
    return Status::OK();
  }
  Status Unlink(InodeID, std::string_view, std::optional<InodeID> = std::nullopt) override {
    return Status::OK();
  }
  Status RmDir(InodeID, std::string_view) override {
    return Status::OK();
  }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, RenameFlag) override {
    return Status::OK();
  }
  Status SetAttr(InodeID ino, const SwordFsAttr &attr, SetAttrField fields, SwordFsInode *out) override {
    if (!set_attr_status.ok() && !set_attr_commit_on_error) {
      return set_attr_status;
    }
    if (HasSetAttrField(fields, SetAttrField::kSize)) {
      file_size_ = static_cast<off_t>(attr.size);
      TruncateChunks(ino, attr.size);
    }
    if (out) {
      *out = {};
      out->attr.size = file_size_;
    }
    return set_attr_status;
  }
  Status SetInodeFlags(InodeID, swordfs::metadata::InodeFlag, SwordFsInode *) override {
    return Status::NotSupported("inode flags");
  }
  Status SetXAttr(InodeID, std::string_view, std::string_view, swordfs::metadata::XAttrSetMode) override {
    return Status::NotSupported("xattr");
  }
  Status GetXAttr(InodeID, std::string_view, std::string *) override {
    return Status::NotSupported("xattr");
  }
  Status ListXAttrs(InodeID, std::vector<std::string> *) override {
    return Status::NotSupported("xattr");
  }
  Status RemoveXAttr(InodeID, std::string_view) override {
    return Status::NotSupported("xattr");
  }
  Status StatFs(SwordFsStatFs *) override {
    return Status::OK();
  }
  Status Symlink(InodeID, std::string_view, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Link(InodeID, InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status Readlink(InodeID, std::string *) override {
    return Status::OK();
  }
  Status Open(InodeID, uint64_t *size = nullptr, swordfs::metadata::InodeFlag * = nullptr) override {
    if (size != nullptr) {
      *size = static_cast<uint64_t>(file_size_);
    }
    return Status::OK();
  }
  Status PrepareReclaim(InodeID) override {
    return Status::OK();
  }
  Status CompleteReclaim(InodeID) override {
    return Status::OK();
  }
  Status VisitOrphanCandidates(const swordfs::metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const swordfs::metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingDeletesBatch(size_t max_items, const swordfs::metadata::PendingDeleteVisitorFn &visitor,
                                  bool *has_more) override {
    std::vector<swordfs::metadata::PendingDelete> pending;
    pending.reserve(pending_deletes_.size());
    for (const auto &[key, work] : pending_deletes_) {
      (void)key;
      pending.push_back(work);
    }
    *has_more = pending.size() > max_items;
    const size_t count = std::min(max_items, pending.size());
    for (size_t i = 0; i < count; ++i) {
      auto status = visitor(pending[i]);
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }
  Status CompletePendingDelete(std::string_view key) override {
    complete_pending_delete_calls.emplace_back(key);
    if (!complete_pending_delete_status.ok()) {
      return complete_pending_delete_status;
    }
    pending_deletes_.erase(std::string(key));
    return Status::OK();
  }
  Status AllocateChunkRevision(swordfs::metadata::ChunkRevision *revision) override {
    ++allocate_chunk_revision_calls;
    if (!allocate_chunk_revision_status.ok()) {
      return allocate_chunk_revision_status;
    }
    if (revision == nullptr) {
      return Status::InvalidArgument("chunk revision output is null");
    }
    *revision = next_revision_++;
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }

  Status SeedChunkForTest(InodeID ino, const SwordFsChunk &chunk) {
    chunks_[ino][chunk.index] = chunk;
    if (chunk.revision >= next_revision_) {
      next_revision_ = chunk.revision + 1;
    }
    return Status::OK();
  }

  // Install an actual typed FileMetadata attachment and COW-private head.
  // Unlike the legacy descriptor seeding helper, readers will resolve this
  // identity through the same authority chain as production.
  Status SeedTypedChunkForTest(InodeID ino, ChunkIndex index, size_t size, std::string *object_key,
                               bool allow_malformed_size = false) {
    if (object_key == nullptr || (size > kTestChunkSize && !allow_malformed_size)) {
      return Status::InvalidArgument("invalid typed chunk seed request");
    }
    swordfs::metadata::ChunkID id;
    auto status = cow_metadata_->AllocateChunkID(&id);
    if (!status.ok()) {
      return status;
    }
    swordfs::metadata::cow::COWChunkRevision revision;
    status = cow_metadata_->AllocateRevision(id, &revision);
    if (!status.ok()) {
      return status;
    }
    status = cow_metadata_->CompareExchangeHead(id, std::nullopt, {.revision = revision, .size = size});
    if (!status.ok()) {
      return status;
    }
    if (!typed_chunks_[ino].emplace(index, id).second) {
      return Status::AlreadyExists("typed chunk index already attached");
    }
    const swordfs::chunk::cow::COWObjectKey key(id, revision);
    *object_key = std::string(static_cast<std::string_view>(key));
    return Status::OK();
  }

  void SetNextFindChunkResult(const SwordFsChunk &chunk) {
    next_find_chunk_result_ = chunk;
    next_find_chunk_status_.reset();
    if (chunk.revision >= next_revision_) {
      next_revision_ = chunk.revision + 1;
    }
  }

  void SetNextFindChunkStatus(Status status) {
    next_find_chunk_status_ = std::move(status);
    next_find_chunk_result_.reset();
  }

  void EraseChunkForTest(InodeID ino, ChunkIndex index) {
    auto ino_it = chunks_.find(ino);
    if (ino_it == chunks_.end()) {
      return;
    }
    ino_it->second.erase(index);
    if (ino_it->second.empty()) {
      chunks_.erase(ino_it);
    }
  }

  void DetachTypedChunkForTest(InodeID ino, ChunkIndex index) {
    typed_chunks_[ino].erase(index);
  }

  Status CommitChunk(InodeID ino, const std::optional<SwordFsChunk> &expected,
                     const SwordFsChunk &replacement) override {
    if (commit_started_ != nullptr) {
      auto *started = commit_started_;
      auto *release = commit_release_;
      commit_started_ = nullptr;
      commit_release_ = nullptr;
      started->post();
      release->wait();
    }
    if (expected.has_value() && replacement.revision <= expected->revision) {
      return Status::InvalidArgument("replacement revision must increase");
    }
    if (!expected.has_value()) {
      if (!publish_chunk_status.ok() && !publish_chunk_commit_on_error) {
        if (publish_chunk_status.ToErrno() == EEXIST || publish_chunk_status.IsNotFound()) {
          QueuePendingDelete(ino, replacement);
        }
        return publish_chunk_status;
      }

      auto &chunk_map = chunks_[ino];
      auto it = chunk_map.find(replacement.index);
      if (it != chunk_map.end()) {
        if (!(it->second == replacement)) {
          QueuePendingDelete(ino, replacement);
          return Status::AlreadyExists("conflicting chunk");
        }
      } else {
        chunk_map.emplace(replacement.index, replacement);
      }
      file_size_ = std::max(
          file_size_, static_cast<off_t>(static_cast<uint64_t>(replacement.index) * kTestChunkSize + replacement.size));
      return publish_chunk_status;
    }

    ++replace_chunk_calls;
    if (!replace_chunk_status.ok() && !replace_chunk_commit_on_error && !replace_chunk_descriptor_only_on_error) {
      if (replace_chunk_status.ToErrno() == EEXIST || replace_chunk_status.IsNotFound()) {
        QueuePendingDelete(ino, replacement);
      }
      return replace_chunk_status;
    }
    auto &chunk_map = chunks_[ino];
    auto it = chunk_map.find(expected->index);
    if (it == chunk_map.end()) {
      QueuePendingDelete(ino, replacement);
      return Status::NotFound("chunk not found");
    }
    if (it->second == replacement) {
      QueuePendingDelete(ino, *expected);
      file_size_ = std::max(
          file_size_, static_cast<off_t>(static_cast<uint64_t>(replacement.index) * kTestChunkSize + replacement.size));
      return replace_chunk_status;
    }
    if (!(it->second == *expected)) {
      QueuePendingDelete(ino, replacement);
      return Status::AlreadyExists("chunk changed before replacement");
    }
    QueuePendingDelete(ino, *expected);
    it->second = replacement;
    if (!replace_chunk_descriptor_only_on_error) {
      file_size_ = std::max(
          file_size_, static_cast<off_t>(static_cast<uint64_t>(replacement.index) * kTestChunkSize + replacement.size));
    }
    return replace_chunk_status;
  }

  Status FindChunk(InodeID ino, ChunkIndex idx, SwordFsChunk *chunk) override {
    ++find_chunk_calls;
    if (next_find_chunk_status_.has_value()) {
      auto status = std::move(*next_find_chunk_status_);
      next_find_chunk_status_.reset();
      return status;
    }
    if (next_find_chunk_result_.has_value()) {
      if (chunk) {
        *chunk = *next_find_chunk_result_;
      }
      next_find_chunk_result_.reset();
      return Status::OK();
    }
    if (!find_chunk_status.ok() && (!find_chunk_error_idx.has_value() || *find_chunk_error_idx == idx)) {
      return find_chunk_status;
    }
    auto it = chunks_.find(ino);
    if (it == chunks_.end()) {
      return Status::NotFound("");
    }
    auto cit = it->second.find(idx);
    if (cit == it->second.end()) {
      return Status::NotFound("");
    }
    if (chunk) {
      *chunk = cit->second;
    }
    return Status::OK();
  }

  Status Truncate(InodeID ino, uint64_t size) override {
    ++truncate_calls;
    if (!truncate_status_.ok() && !truncate_commit_on_error) {
      return truncate_status_;
    }
    TruncateChunks(ino, size);
    file_size_ = static_cast<off_t>(size);
    return truncate_status_;
  }

  void set_file_size(off_t size) {
    file_size_ = size;
  }
  void set_reported_file_size(uint64_t size) {
    reported_file_size_ = size;
  }
  void set_truncate_status(Status s) {
    truncate_status_ = s;
  }
  off_t file_size() const {
    return file_size_;
  }

  void BlockNextCommit(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    commit_started_ = started;
    commit_release_ = release;
  }

  void PauseNextPublicationIfRequested() {
    if (commit_started_ == nullptr) {
      return;
    }
    auto *started = std::exchange(commit_started_, nullptr);
    auto *release = std::exchange(commit_release_, nullptr);
    started->post();
    release->wait();
  }

  void BlockNextGetInode(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    get_inode_started_ = started;
    get_inode_release_ = release;
  }

  std::vector<std::string> PendingDeleteKeys() const {
    std::vector<std::string> out;
    out.reserve(pending_deletes_.size());
    for (const auto &[id, pending] : pending_deletes_) {
      (void)id;
      swordfs::chunk::cow::COWRef ref;
      auto status = swordfs::chunk::cow::DecodeCOWDelete(pending, 0, &ref);
      EXPECT_TRUE(status.ok()) << status.message();
      if (status.ok()) {
        out.push_back(ref.key);
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }

  int truncate_calls = 0;
  int find_chunk_calls = 0;
  int replace_chunk_calls = 0;
  int allocate_chunk_revision_calls = 0;
  std::vector<std::string> complete_pending_delete_calls;
  Status complete_pending_delete_status = Status::OK();
  Status allocate_chunk_revision_status = Status::OK();
  Status publish_chunk_status = Status::OK();
  bool publish_chunk_commit_on_error = false;
  Status attach_prepared_status = Status::OK();
  bool attach_prepared_commit_on_error = false;
  Status finalize_attached_status = Status::OK();
  bool finalize_attached_commit_on_error = false;
  Status replace_chunk_status = Status::OK();
  bool replace_chunk_commit_on_error = false;
  bool replace_chunk_descriptor_only_on_error = false;
  Status set_attr_status = Status::OK();
  bool set_attr_commit_on_error = false;
  bool truncate_commit_on_error = false;
  Status find_chunk_status = Status::OK();
  Status get_inode_status = Status::OK();
  std::optional<ChunkIndex> find_chunk_error_idx;

 private:
  void QueuePendingDelete(InodeID ino, const SwordFsChunk &chunk) {
    swordfs::metadata::PendingDelete work;
    auto status = swordfs::chunk::cow::FreezeCOWDelete(ino, chunk, 0, &work);
    EXPECT_TRUE(status.ok()) << status.message();
    if (status.ok()) {
      pending_deletes_.insert_or_assign(work.id, std::move(work));
    }
  }

  void TruncateChunks(InodeID ino, uint64_t size) {
    auto ino_it = chunks_.find(ino);
    if (ino_it == chunks_.end()) {
      return;
    }
    for (auto it = ino_it->second.begin(); it != ino_it->second.end();) {
      auto &chunk = it->second;
      const uint64_t start_offset = static_cast<uint64_t>(chunk.index) * kTestChunkSize;
      if (start_offset >= size) {
        QueuePendingDelete(ino, chunk);
        it = ino_it->second.erase(it);
        continue;
      }
      const uint64_t max_size = size - start_offset;
      if (chunk.size > max_size) {
        chunk.size = max_size;
      }
      ++it;
    }
  }

 private:
  off_t file_size_ = 0;
  std::unordered_map<InodeID, std::map<ChunkIndex, swordfs::metadata::ChunkID>> typed_chunks_;
  std::shared_ptr<FaultingCOWChunkMetadata> cow_metadata_ = std::make_shared<FaultingCOWChunkMetadata>();
  std::optional<uint64_t> reported_file_size_;
  Status truncate_status_ = Status::OK();
  swordfs::metadata::ChunkRevision next_revision_ = 1;
  std::unordered_map<InodeID, std::unordered_map<ChunkIndex, SwordFsChunk>> chunks_;
  std::unordered_map<std::string, swordfs::metadata::PendingDelete> pending_deletes_;
  std::optional<SwordFsChunk> next_find_chunk_result_;
  std::optional<Status> next_find_chunk_status_;
  folly::fibers::Baton *get_inode_started_{nullptr};
  folly::fibers::Baton *get_inode_release_{nullptr};
  folly::fibers::Baton *commit_started_{nullptr};
  folly::fibers::Baton *commit_release_{nullptr};
};

// ────────────────────────────────────────────────────────────────
// FileReadWriterTest
// ────────────────────────────────────────────────────────────────

class FileReadWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto data = std::make_unique<MockDataEngine>();
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<MockMetaEngine>>();
    mock_data_ = data.get();
    mock_meta_ = meta.get();
    SwordFsVolume config;
    config.chunk_size = kChunkSize;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    cow_metadata_ = mock_meta_->CowMetadata();
  }

  FileReadWriter Make(off_t file_size = 0) {
    mock_meta_->set_file_size(file_size);
    return FileReadWriter(kIno, kMaxParallelFlushes);
  }

  Status SeedPersistedChunk(ChunkIndex index, std::string_view payload, size_t visible_size) {
    std::string key;
    auto status = mock_meta_->SeedTypedChunkForTest(kIno, index, visible_size, &key);
    if (!status.ok()) {
      return status;
    }
    return mock_data_->Put(key, std::make_unique<folly::IOBuf>(Buf(std::string(payload))));
  }

  static constexpr size_t kChunkSize = kTestChunkSize;
  static constexpr InodeID kIno = 42;
  std::shared_ptr<FaultingCOWChunkMetadata> cow_metadata_;
  MockDataEngine *mock_data_ = nullptr;
  MockMetaEngine *mock_meta_ = nullptr;
};

// ────────────────────────────────────────────────────────────────
// Write → Read round-trip (dirty buffer)
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, FullChunk) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('A', kChunkSize));
  });
}

TEST_F(FileReadWriterTest, WithinChunkWithOffset) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(100, 200, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('A', 100));
  });
}

TEST_F(FileReadWriterTest, PartialChunkAtEOF) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', 500)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 500);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('A', 500));
  });
}

TEST_F(FileReadWriterTest, PersistedPartialChunkAtEOFReturnsShortRead) {
  RunInTestFiber([&] {
    ASSERT_TRUE(SeedPersistedChunk(0, Repeat('P', 300), 300).ok());
    auto rw = Make(300);
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 300);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('P', 300));
  });
}

TEST_F(FileReadWriterTest, PastEOF) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(100, kChunkSize + 100, out.get()).ok());
    EXPECT_EQ(out->length(), 0);
  });
}

// ────────────────────────────────────────────────────────────────
// Cross-chunk reads
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, CrossChunkBoundary) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 500)), static_cast<off_t>(kChunkSize)).ok());

    off_t off = static_cast<off_t>(kChunkSize) - 100;
    std::string expected = Repeat('A', 100) + Repeat('B', 500);
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(600, off, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);
  });
}

TEST_F(FileReadWriterTest, CrossChunkExactBoundary) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 500)), static_cast<off_t>(kChunkSize)).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(500, static_cast<off_t>(kChunkSize), out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('B', 500));
  });
}

TEST_F(FileReadWriterTest, CrossChunkReadsIntoSecondChunk) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 800)), static_cast<off_t>(kChunkSize)).ok());

    off_t off = static_cast<off_t>(kChunkSize) - 1;
    std::string expected = "A" + Repeat('B', 500);
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(501, off, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);
  });
}

TEST_F(FileReadWriterTest, CrossChunkExhaustsSecondChunk) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 200)), static_cast<off_t>(kChunkSize)).ok());

    off_t off = static_cast<off_t>(kChunkSize) - 50;
    auto out = folly::IOBuf::create(kChunkSize * 2);
    ASSERT_TRUE(rw.Read(kChunkSize, off, out.get()).ok());
    std::string expected = Repeat('A', 50) + Repeat('B', 200);
    EXPECT_EQ(out->length(), expected.size());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);
  });
}

TEST_F(FileReadWriterTest, CrossChunkSecondChunkMissing) {
  RunInTestFiber([&] {
    auto rw = Make(kChunkSize + 150);
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());

    off_t off = static_cast<off_t>(kChunkSize) - 50;
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(200, off, out.get()).ok());
    std::string expected = Repeat('A', 50) + std::string(150, '\0');
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);
  });
}

// ────────────────────────────────────────────────────────────────
// Edge cases
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, ZeroSizeRequest) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(0, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, NegativeReadOffsetIsRejected) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    auto out = folly::IOBuf::create(64);

    auto status = rw.Read(1, -1, out.get());

    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(status.message(), "FileReadWriter::Read: negative offset");
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, ZeroLengthNegativeReadOffsetIsRejected) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    auto out = folly::IOBuf::create(1);

    const auto status = rw.Read(0, -1, out.get());

    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(mock_meta_->find_chunk_calls, 0);
  });
}

TEST_F(FileReadWriterTest, NegativeWriteOffsetIsRejectedBeforeChunkLookup) {
  RunInTestFiber([&] {
    auto rw = Make();

    const auto status = rw.Write(Buf("x"), -1);

    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(mock_meta_->find_chunk_calls, 0);
    EXPECT_TRUE(mock_data_->StoredKeys().empty());
    EXPECT_EQ(mock_meta_->file_size(), 0);
  });
}

TEST_F(FileReadWriterTest, HighOffsetWriteFlushAndFreshReadPreserveLogicalIndex) {
  RunInTestFiber([&] {
    constexpr off_t kWriteOffset = std::numeric_limits<off_t>::max() - 1;
    const uint64_t expected_index = static_cast<uint64_t>(kWriteOffset) / kChunkSize;
    ASSERT_GT(expected_index, std::numeric_limits<uint32_t>::max());

    auto rw = Make();
    auto status = rw.Write(Buf("X"), kWriteOffset);
    ASSERT_TRUE(status.ok()) << status.message();
    status = rw.Flush();
    ASSERT_TRUE(status.ok()) << status.message();

    EXPECT_EQ(mock_meta_->file_size(), std::numeric_limits<off_t>::max());
    swordfs::metadata::FileChunkSnapshot snapshot;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, expected_index, &snapshot).ok());
    ASSERT_TRUE(snapshot.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead head;
    ASSERT_TRUE(cow_metadata_->GetHead(*snapshot.chunk_id, &head).ok());
    const swordfs::chunk::cow::COWObjectKey key(*snapshot.chunk_id, head.revision);
    ASSERT_EQ(mock_data_->StoredKeys(), std::vector<std::string>{std::string(static_cast<std::string_view>(key))});

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(2);
    status = reopened.Read(2, kWriteOffset - 1, out.get());
    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_EQ(out->length(), 2U);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), std::string("\0X", 2));
  });
}

TEST_F(FileReadWriterTest, WritePastSupportedFileSizeIsRejectedBeforeMutation) {
  RunInTestFiber([&] {
    constexpr off_t kWriteOffset = std::numeric_limits<off_t>::max() - 1;
    auto rw = Make();

    const auto status = rw.Write(Buf("XX"), kWriteOffset);

    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(mock_meta_->find_chunk_calls, 0);
    EXPECT_TRUE(mock_data_->StoredKeys().empty());
    EXPECT_EQ(mock_meta_->file_size(), 0);
  });
}

TEST_F(FileReadWriterTest, VisibleSizeMetadataFailurePropagatesFromRead) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    mock_meta_->get_inode_status = Status::IOError("injected inode-size lookup failure");
    auto out = folly::IOBuf::create(64);

    auto status = rw.Read(64, 0, out.get());

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "injected inode-size lookup failure");
    EXPECT_EQ(out->length(), 0);
    EXPECT_EQ(mock_meta_->find_chunk_calls, 0);
  });
}

TEST_F(FileReadWriterTest, ReadRejectsPersistedSizeBeyondSupportedFileRange) {
  RunInTestFiber([&] {
    auto rw = Make();
    mock_meta_->set_reported_file_size(swordfs::metadata::kMaxSupportedFileSize + 1);
    auto out = folly::IOBuf::create(1);

    const auto status = rw.Read(1, 0, out.get());

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "FileReadWriter::Read: file size exceeds supported range");
    EXPECT_EQ(out->length(), 0);
    EXPECT_EQ(mock_meta_->find_chunk_calls, 0);
  });
}

TEST_F(FileReadWriterTest, VisibleSizeSnapshotCannotRegressBehindCompletedFlush) {
  auto rw = Make();

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton get_inode_started;
  folly::fibers::Baton release_get_inode;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton read_done;
  mock_meta_->BlockNextGetInode(&get_inode_started, &release_get_inode);

  Status read_status;
  Status write_status;
  Status flush_status;
  auto out = folly::IOBuf::create(13);
  fm.addTask([&] {
    read_status = rw.Read(13, 0, out.get());
    read_done.post();
  });
  fm.addTask([&] {
    get_inode_started.wait();
    write_status = rw.Write(Buf("Hello,_World!"), 0);
    if (write_status.ok()) {
      flush_status = rw.Flush();
    }
    flush_done.post();
  });

  const bool flush_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return flush_done.try_wait(); });
  EXPECT_TRUE(flush_completed) << "concurrent flush must complete while the read snapshot is blocked";
  if (!flush_completed) {
    release_get_inode.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && read_done.try_wait(); },
        "FileReadWriter visible-size timeout cleanup");
    return;
  }
  EXPECT_TRUE(write_status.ok()) << write_status.message();
  EXPECT_TRUE(flush_status.ok()) << flush_status.message();
  EXPECT_EQ(mock_meta_->file_size(), 13);

  release_get_inode.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return read_done.try_wait(); }, "FileReadWriter blocked read completion after snapshot release");
  ASSERT_TRUE(read_status.ok()) << read_status.message();
  EXPECT_EQ(out->length(), 13U);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello,_World!");
}

TEST_F(FileReadWriterTest, EmptyOutputOnNoData) {
  RunInTestFiber([&] {
    auto rw = Make();
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(64, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, MissingChunkMetadataReadsAsSparseZeros) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    // No FileMetadata attachment at this index is a logical hole. A backend
    // read error is different and must still propagate to the caller.

    auto out = folly::IOBuf::create(64);
    auto status = rw.Read(64, 0, out.get());

    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_EQ(out->length(), 64);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), std::string(64, '\0'));
  });
}

TEST_F(FileReadWriterTest, MaterializedPublishedChunkZeroFillsItsUncoveredTail) {
  RunInTestFiber([&] {
    ASSERT_TRUE(SeedPersistedChunk(0, "hello", 5).ok());

    auto rw = Make(/*size=*/8);
    auto out = folly::IOBuf::create(8);
    const auto status = rw.Read(8, 0, out.get());

    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_EQ(out->length(), 8U);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), 5), "hello");
    EXPECT_EQ(out->data()[5], 0U);
    EXPECT_EQ(out->data()[6], 0U);
    EXPECT_EQ(out->data()[7], 0U);
  });
}

TEST_F(FileReadWriterTest, PersistedChunkShortReadFailsClosed) {
  RunInTestFiber([&] {
    ASSERT_TRUE(SeedPersistedChunk(0, Repeat('S', 32), 64).ok());
    auto rw = Make(64);
    auto out = folly::IOBuf::create(64);
    auto status = rw.Read(64, 0, out.get());

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, PersistedChunkReadErrorRollsBackPartialOutput) {
  RunInTestFiber([&] {
    SwordFsChunk published{};
    published.index = 0;
    published.revision = 18;
    published.size = 64;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, published).ok());

    mock_data_->get_error_payload = "partial";
    mock_data_->get_status = Status::IOError("injected data read failure");

    swordfs::chunk::cow::COWChunk chunk(
        kIno, 0, kTestChunkSize, cow_metadata_, mock_meta_, mock_data_, swordfs::metadata::ChunkID(70),
        swordfs::metadata::cow::COWChunkHead{.revision = swordfs::metadata::cow::COWChunkRevision(published.revision),
                                             .size = published.size},
        published.size);

    auto out = folly::IOBuf::create(68);
    std::memcpy(out->writableTail(), "keep", 4);
    out->append(4);
    auto status = chunk.Read(0, published.size, out.get());

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "injected data read failure");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "keep");
  });
}

TEST_F(FileReadWriterTest, PersistedChunkZeroLengthReadIsNoOp) {
  RunInTestFiber([&] {
    SwordFsChunk published{};
    published.index = 0;
    published.revision = 19;
    published.size = 64;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, published).ok());

    swordfs::chunk::cow::COWChunk chunk(
        kIno, 0, kTestChunkSize, cow_metadata_, mock_meta_, mock_data_, swordfs::metadata::ChunkID(71),
        swordfs::metadata::cow::COWChunkHead{.revision = swordfs::metadata::cow::COWChunkRevision(published.revision),
                                             .size = published.size},
        published.size);

    auto out = folly::IOBuf::copyBuffer("keep");
    ASSERT_TRUE(chunk.Read(16, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "keep");
  });
}

TEST_F(FileReadWriterTest, ChunkMetadataIOErrorPropagatesFromRead) {
  RunInTestFiber([&] {
    // Keep the request inside logical EOF so the read must resolve chunk
    // metadata instead of correctly terminating at EOF first.
    auto rw = Make(64);
    mock_meta_->find_chunk_status = Status::IOError("injected metadata read failure");

    auto out = folly::IOBuf::create(64);
    auto status = rw.Read(64, 0, out.get());

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "injected metadata read failure");
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, MalformedChunkMetadataPropagatesFromRead) {
  RunInTestFiber([&] {
    // Keep the request inside logical EOF so malformed chunk metadata remains
    // observable rather than being bypassed by the EOF boundary.
    auto rw = Make(64);
    mock_meta_->find_chunk_status = Status::Malformed("injected malformed chunk metadata");

    auto out = folly::IOBuf::create(64);
    auto status = rw.Read(64, 0, out.get());

    EXPECT_TRUE(status.ToErrno() == EIO);
    EXPECT_EQ(status.message(), "injected malformed chunk metadata");
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, ChunkMetadataIOErrorPropagatesFromWrite) {
  RunInTestFiber([&] {
    auto rw = Make();
    mock_meta_->find_chunk_status = Status::IOError("injected metadata write lookup failure");

    auto status = rw.Write(Buf("data"), 0);

    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "injected metadata write lookup failure");
  });
}

TEST_F(FileReadWriterTest, CrossChunkMetadataErrorDrainsSubmittedReadBeforeReturning) {
  RunInTestFiber([&] { ASSERT_TRUE(SeedPersistedChunk(0, Repeat('R', kChunkSize), kChunkSize).ok()); });

  mock_meta_->find_chunk_status = Status::IOError("injected second-chunk metadata failure");
  mock_meta_->find_chunk_error_idx = 1;

  auto rw = Make(kChunkSize * 2);
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton get_started;
  folly::fibers::Baton release_get;
  folly::fibers::Baton read_done;
  mock_data_->BlockNextGet(&get_started, &release_get);

  Status read_status;
  auto out = folly::IOBuf::create(kChunkSize * 2);
  fm.addTask([&] {
    read_status = rw.Read(kChunkSize * 2, 0, out.get());
    read_done.post();
  });

  const bool get_observed = swordfs::test::DriveEventBaseUntil(evb, [&] { return get_started.try_wait(); });
  EXPECT_TRUE(get_observed) << "first chunk Get must reach the controlled blocker";
  if (!get_observed) {
    release_get.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return read_done.try_wait(); }, "FileReadWriter blocked Get timeout cleanup");
    return;
  }
  EXPECT_FALSE(read_done.try_wait());

  release_get.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return read_done.try_wait(); }, "FileReadWriter partial-read completion after Get release");

  EXPECT_EQ(read_status.ToErrno(), EIO);
  EXPECT_EQ(read_status.message(), "injected second-chunk metadata failure");
  EXPECT_EQ(out->length(), 0);
}

TEST_F(FileReadWriterTest, SparseReadWithMultipleHoles) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', kChunkSize)), static_cast<off_t>(kChunkSize * 3)).ok());

    auto out = folly::IOBuf::create(kChunkSize * 4);
    ASSERT_TRUE(rw.Read(kChunkSize * 4, 0, out.get()).ok());
    std::string expected = Repeat('A', kChunkSize) + std::string(kChunkSize * 2, '\0') + Repeat('B', kChunkSize);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);
  });
}

// ────────────────────────────────────────────────────────────────
// Flush publication and retry semantics
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, FlushRetriesPutFailureUntilDataIsPublished) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());

    mock_data_->put_status = Status::IOError("injected put failure");
    EXPECT_FALSE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot unpublished;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &unpublished).ok());
    EXPECT_FALSE(unpublished.chunk_id.has_value());
    EXPECT_TRUE(mock_data_->StoredKeys().empty());
    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->file_size(), 6);

    swordfs::metadata::FileChunkSnapshot committed;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &committed).ok());
    ASSERT_TRUE(committed.chunk_id.has_value());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reopened.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
  });
}

TEST_F(FileReadWriterTest, PutFailureRetryPublishesNewTypedAttachment) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    mock_data_->put_status = Status::IOError("injected put failure");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    swordfs::metadata::FileChunkSnapshot missing;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &missing).ok());
    EXPECT_FALSE(missing.chunk_id.has_value());

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    EXPECT_EQ(published.inode.attr.size, 5U);
    EXPECT_EQ(mock_data_->StoredKeys().size(), 1U);
  });
}

TEST_F(FileReadWriterTest, SuccessfulFirstFlushIsIdempotentWithoutAdditionalWrites) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 1);
    swordfs::metadata::FileChunkSnapshot first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &first).ok());
    ASSERT_TRUE(first.chunk_id.has_value());
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot again;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &again).ok());
    EXPECT_EQ(again.chunk_id, first.chunk_id);
    EXPECT_EQ(mock_data_->put_calls, 1);
  });
}

TEST_F(FileReadWriterTest, FlushRetriesRevisionAllocationFailureBeforeUploadingObject) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());

    cow_metadata_->allocate_revision_status = Status::IOError("typed COW revision allocation failed");
    const auto failed = rw.Flush();
    EXPECT_EQ(failed.ToErrno(), EIO);
    EXPECT_EQ(cow_metadata_->allocate_revision_calls, 1);
    EXPECT_EQ(mock_data_->put_calls, 0);
    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    cow_metadata_->allocate_revision_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(cow_metadata_->allocate_revision_calls, 2);
    EXPECT_EQ(mock_data_->put_calls, 1);
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead head;
    ASSERT_TRUE(cow_metadata_->GetHead(*published.chunk_id, &head).ok());
    EXPECT_NE(head.revision, swordfs::metadata::cow::kInvalidCOWChunkRevision);
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reopened.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
  });
}

TEST_F(FileReadWriterTest, CompetingInitialPublishersCannotReplaceWinningTypedAttachment) {
  RunInTestFiber([&] {
    FileReadWriter first(kIno, kMaxParallelFlushes);
    FileReadWriter second(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(first.Write(Buf("first"), 0).ok());
    ASSERT_TRUE(second.Write(Buf("second"), 0).ok());

    ASSERT_TRUE(first.Flush().ok());
    swordfs::metadata::FileChunkSnapshot winner;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &winner).ok());
    ASSERT_TRUE(winner.chunk_id.has_value());

    const auto status = second.Flush();
    EXPECT_EQ(status.ToErrno(), EEXIST);
    swordfs::metadata::FileChunkSnapshot still_winner;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &still_winner).ok());
    EXPECT_EQ(still_winner.chunk_id, winner.chunk_id);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reader.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "first");
  });
}

TEST_F(FileReadWriterTest, FlushRetriesPublicationFailureWithFreshRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    const std::string payload = Repeat('M', 128);
    ASSERT_TRUE(rw.Write(Buf(payload), 0).ok());

    mock_meta_->attach_prepared_status = Status::IOError("injected typed attachment failure");
    EXPECT_FALSE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot unpublished;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &unpublished).ok());
    EXPECT_FALSE(unpublished.chunk_id.has_value());
    EXPECT_EQ(mock_data_->StoredKeys().size(), 1U);
    EXPECT_FALSE(rw.Flush().ok());
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &unpublished).ok());
    EXPECT_FALSE(unpublished.chunk_id.has_value());
    EXPECT_EQ(mock_data_->StoredKeys().size(), 2U);

    mock_meta_->attach_prepared_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->StoredKeys().size(), 3U);
    EXPECT_EQ(mock_meta_->file_size(), static_cast<off_t>(payload.size()));
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead head;
    ASSERT_TRUE(cow_metadata_->GetHead(*published.chunk_id, &head).ok());
    const swordfs::chunk::cow::COWObjectKey key(*published.chunk_id, head.revision);
    const auto stored_keys = mock_data_->StoredKeys();
    EXPECT_NE(std::find(stored_keys.begin(), stored_keys.end(), std::string(static_cast<std::string_view>(key))),
              stored_keys.end());

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(payload.size());
    ASSERT_TRUE(reopened.Read(payload.size(), 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), payload);
  });
}

TEST_F(FileReadWriterTest, AmbiguousUncommittedTypedAttachmentRetriesWithFreshChunkIdentity) {
  RunInTestFiber([&] {
    auto rw = Make();
    const std::string payload = Repeat('A', 128);
    ASSERT_TRUE(rw.Write(Buf(payload), 0).ok());

    mock_meta_->attach_prepared_status = Status::OutcomeUnknown("lost typed attachment response, not committed");
    mock_meta_->attach_prepared_commit_on_error = false;
    EXPECT_TRUE(rw.Flush().IsOutcomeUnknown());
    ASSERT_EQ(mock_data_->put_calls, 1);
    swordfs::metadata::FileChunkSnapshot absent;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &absent).ok());
    EXPECT_FALSE(absent.chunk_id.has_value());
    const auto discarded_keys = mock_data_->StoredKeys();
    ASSERT_EQ(discarded_keys.size(), 1U);

    mock_meta_->attach_prepared_status = Status::OK();
    EXPECT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 2);
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead head;
    ASSERT_TRUE(cow_metadata_->GetHead(*published.chunk_id, &head).ok());
    const swordfs::chunk::cow::COWObjectKey published_key(*published.chunk_id, head.revision);
    EXPECT_NE(discarded_keys.front(), std::string(static_cast<std::string_view>(published_key)));
    EXPECT_EQ(published.inode.attr.size, payload.size());
  });
}

TEST_F(FileReadWriterTest, DirtyUnattachedWriterRejectsCompetingTypedAttachment) {
  RunInTestFiber([&] {
    auto rw = Make();
    std::string payload = Repeat('C', 128);
    ASSERT_TRUE(rw.Write(Buf(payload), 0).ok());

    mock_meta_->attach_prepared_status = Status::IOError("injected first-attachment failure");
    EXPECT_FALSE(rw.Flush().ok());
    ASSERT_EQ(mock_data_->put_calls, 1);
    ASSERT_TRUE(rw.Write(Buf("L"), 0).ok());
    payload[0] = 'L';

    ASSERT_TRUE(SeedPersistedChunk(0, Repeat('E', 128), 128).ok());
    mock_meta_->set_file_size(128);
    swordfs::metadata::FileChunkSnapshot competing;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &competing).ok());
    ASSERT_TRUE(competing.chunk_id.has_value());

    mock_meta_->attach_prepared_status = Status::OK();
    EXPECT_EQ(rw.Flush().ToErrno(), EEXIST)
        << "dirty data prepared for a hole must not migrate into another writer's ChunkID";
    swordfs::metadata::FileChunkSnapshot still_competing;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &still_competing).ok());
    EXPECT_EQ(still_competing.chunk_id, competing.chunk_id);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(payload.size());
    ASSERT_TRUE(reopened.Read(payload.size(), 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('E', 128));
  });
}

TEST_F(FileReadWriterTest, RejectedTypedAttachmentRetryUsesFreshImmutableIdentity) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());

    mock_meta_->attach_prepared_status = Status::AlreadyExists("injected first attachment rejection");
    ASSERT_EQ(rw.Flush().ToErrno(), EEXIST);
    const auto rejected_keys = mock_data_->StoredKeys();
    ASSERT_EQ(rejected_keys.size(), 1U);
    swordfs::metadata::FileChunkSnapshot unattached;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &unattached).ok());
    EXPECT_FALSE(unattached.chunk_id.has_value());
    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());

    // The failed attempt's candidate object must never become authoritative
    // on a later successful retry; its immutable ChunkID is retired.
    mock_meta_->attach_prepared_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot current;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &current).ok());
    ASSERT_TRUE(current.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead head;
    ASSERT_TRUE(cow_metadata_->GetHead(*current.chunk_id, &head).ok());
    const swordfs::chunk::cow::COWObjectKey current_key(*current.chunk_id, head.revision);
    const auto stored_keys = mock_data_->StoredKeys();
    ASSERT_EQ(stored_keys.size(), 2U);
    EXPECT_NE(rejected_keys.front(), std::string(static_cast<std::string_view>(current_key)));
    EXPECT_NE(
        std::find(stored_keys.begin(), stored_keys.end(), std::string(static_cast<std::string_view>(current_key))),
        stored_keys.end());

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(5);
    ASSERT_TRUE(reopened.Read(5, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello");
  });
}

TEST_F(FileReadWriterTest, FirstAttachmentRetriesWhenEofBoundaryAttachesWithoutSizeChange) {
  // A sibling chunk may materialize inside the existing EOF while this
  // chunk's first publication is in flight. The EOF remains unchanged, but
  // the boundary identity observed in the initial snapshot becomes stale.
  const uint64_t existing_eof = 2 * kChunkSize + 8;
  auto rw = Make(existing_eof);
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("new"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton commit_started;
  folly::fibers::Baton release_commit;
  folly::fibers::Baton flush_done;
  mock_meta_->BlockNextCommit(&commit_started, &release_commit);

  Status flush_status;
  Status boundary_status;
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    commit_started.wait();
    boundary_status = SeedPersistedChunk(2, "B", 1);
    release_commit.post();
  });
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait(); }, "first attachment with unchanged EOF boundary race");

  ASSERT_TRUE(boundary_status.ok()) << boundary_status.message();
  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  EXPECT_EQ(mock_meta_->file_size(), existing_eof);
  EXPECT_EQ(mock_data_->StoredKeys().size(), 3U)
      << "the rejected candidate must not be reused after its boundary precondition fails";

  RunInTestFiber([&] {
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(3);
    ASSERT_TRUE(reopened.Read(3, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "new");
  });
}

TEST_F(FileReadWriterTest, RejectedTypedRewriteRetriesWithFreshPrivateRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &first).ok());
    ASSERT_TRUE(first.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead before;
    ASSERT_TRUE(cow_metadata_->GetHead(*first.chunk_id, &before).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_status = Status::AlreadyExists("typed write finalization was rejected");
    ASSERT_TRUE(rw.Flush().ToErrno() == EEXIST);
    const auto uploaded_keys = mock_data_->StoredKeys();
    ASSERT_EQ(uploaded_keys.size(), 2U);

    mock_meta_->finalize_attached_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot current;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &current).ok());
    ASSERT_TRUE(current.chunk_id.has_value());
    EXPECT_EQ(current.chunk_id, first.chunk_id);
    swordfs::metadata::cow::COWChunkHead after;
    ASSERT_TRUE(cow_metadata_->GetHead(*current.chunk_id, &after).ok());
    EXPECT_GT(after.revision.Value(), before.revision.Value());
    EXPECT_EQ(mock_data_->StoredKeys().size(), 3U);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, FlushNeverShrinksExistingFileSize) {
  RunInTestFiber([&] {
    auto rw = Make(4096);
    ASSERT_TRUE(rw.Write(Buf(Repeat('S', 128)), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileMappingSnapshot file;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &file).ok());
    EXPECT_EQ(file.inode.attr.size, 4096U);
    ASSERT_EQ(file.mappings.size(), 1U);
    EXPECT_EQ(file.mappings[0].index, 0U);
    EXPECT_EQ(mock_meta_->file_size(), 4096);
  });
}

TEST_F(FileReadWriterTest, SuccessfulFlushClearsTransientLiveSize) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("Hello,_World!"), 0).ok());

    SwordFsInode inode;
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    ASSERT_EQ(inode.attr.size, 13U);

    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_EQ(mock_meta_->file_size(), 13);

    // Once publication succeeds, metadata is authoritative again. A later
    // size change from outside this local writer must not be masked by an old
    // pre-flush lower bound.
    mock_meta_->set_file_size(5);
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    EXPECT_EQ(inode.attr.size, 5U);
  });
}

TEST_F(FileReadWriterTest, GetAttrSnapshotCannotRegressReadEOFBehindCompletedFlush) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("Hello,_World!"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton get_inode_started;
  folly::fibers::Baton release_get_inode;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton get_attr_done;
  mock_meta_->BlockNextGetInode(&get_inode_started, &release_get_inode);

  Status flush_status;
  Status get_attr_status;
  SwordFsInode inode;
  fm.addTask([&] {
    get_attr_status = rw.GetAttr(&inode);
    get_attr_done.post();
  });
  fm.addTask([&] {
    get_inode_started.wait();
    flush_status = rw.Flush();
    flush_done.post();
  });

  const bool flush_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return flush_done.try_wait(); });
  EXPECT_TRUE(flush_completed) << "flush must complete while GetAttr's metadata snapshot is blocked";
  if (!flush_completed) {
    release_get_inode.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && get_attr_done.try_wait(); },
        "FileReadWriter GetAttr-snapshot timeout cleanup");
    return;
  }
  EXPECT_TRUE(flush_status.ok()) << flush_status.message();
  EXPECT_EQ(mock_meta_->file_size(), 13);

  release_get_inode.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return get_attr_done.try_wait(); }, "FileReadWriter GetAttr completion after snapshot release");
  ASSERT_TRUE(get_attr_status.ok()) << get_attr_status.message();
  EXPECT_EQ(inode.attr.size, 13U);

  RunInTestFiber([&] {
    auto out = folly::IOBuf::create(13);
    ASSERT_TRUE(rw.Read(13, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 13U);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello,_World!");
  });
}

TEST_F(FileReadWriterTest, LiveAttrGuardUsesTransientSize) {
  RunInTestFiber([&] {
    mock_meta_->set_file_size(0);
    InodeHandle handle(kIno);
    ASSERT_TRUE(handle.Write(Buf("Hello,_World!"), 0).ok());
    auto guard = handle.LockLiveAttr();

    SwordFsInode inode;
    inode.ino = kIno;
    inode.attr.ino = kIno;
    inode.attr.size = 1;
    guard.Apply(inode);
    EXPECT_EQ(inode.attr.size, 13U);
  });
}

TEST_F(FileReadWriterTest, GetAttrBlocksDoNotPublishCachedWrite) {
  RunInTestFiber([&] {
    constexpr size_t kWriteSize = 64 * 1024;
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('x', kWriteSize)), 0).ok());

    SwordFsInode inode;
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    struct stat attr{};
    inode.attr.ToPosixStat(&attr);

    EXPECT_EQ(attr.st_size, static_cast<off_t>(kWriteSize));
    EXPECT_EQ(attr.st_blocks, 1);
    EXPECT_EQ(mock_data_->put_calls, 0);
    EXPECT_EQ(mock_meta_->replace_chunk_calls, 0);
    EXPECT_EQ(mock_meta_->file_size(), 0);
  });
}

TEST_F(FileReadWriterTest, SparseCachedWriteKeepsBlocksBelowLogicalSize) {
  RunInTestFiber([&] {
    constexpr off_t kWriteOffset = 1600 * 1024;
    constexpr size_t kWriteSize = 50 * 1024;
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('x', kWriteSize)), kWriteOffset).ok());

    SwordFsInode inode;
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    struct stat attr{};
    inode.attr.ToPosixStat(&attr);

    EXPECT_EQ(attr.st_size, kWriteOffset + static_cast<off_t>(kWriteSize));
    EXPECT_EQ(attr.st_blocks, 1);
    EXPECT_LT(static_cast<uint64_t>(attr.st_blocks) * 512, static_cast<uint64_t>(attr.st_size));
    EXPECT_EQ(mock_data_->put_calls, 0);
    EXPECT_EQ(mock_meta_->replace_chunk_calls, 0);
    EXPECT_EQ(mock_meta_->file_size(), 0);
  });
}

TEST_F(FileReadWriterTest, FailedFlushKeepsTransientLiveSize) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("Hello,_World!"), 0).ok());
    mock_data_->put_status = Status::IOError("injected publication failure");

    auto status = rw.Flush();
    ASSERT_EQ(status.ToErrno(), EIO);
    ASSERT_EQ(mock_meta_->file_size(), 0);

    SwordFsInode inode;
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    EXPECT_EQ(inode.attr.size, 13U);
    struct stat attr{};
    inode.attr.ToPosixStat(&attr);
    EXPECT_EQ(attr.st_blocks, 1);
  });
}

TEST_F(FileReadWriterTest, SuccessfulTruncateDoesNotLeaveTransientSizeOverlay) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("Hello,_World!"), 0).ok());
    ASSERT_TRUE(rw.Truncate(5).ok());

    mock_meta_->set_file_size(2);
    SwordFsInode inode;
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    EXPECT_EQ(inode.attr.size, 2U);
  });
}

TEST_F(FileReadWriterTest, FlushContinuesOtherChunksAfterOneChunkFails) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 128)), static_cast<off_t>(kChunkSize)).ok());

    mock_data_->put_status = Status::IOError("injected first-chunk failure");
    mock_data_->remaining_forced_put_failures = 1;
    mock_data_->fail_put_prefix = "unmatched-candidate/";
    EXPECT_FALSE(rw.Flush().ok());

    swordfs::metadata::FileMappingSnapshot after_partial_flush;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &after_partial_flush).ok());
    EXPECT_EQ(after_partial_flush.mappings.size(), 1U)
        << "a failed chunk must not prevent an independent typed attachment";

    mock_data_->put_status = Status::OK();
    mock_data_->fail_put_prefix.clear();
    EXPECT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileMappingSnapshot after_retry;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &after_retry).ok());
    EXPECT_EQ(after_retry.mappings.size(), 2U);
    EXPECT_EQ(after_retry.inode.attr.size, kChunkSize + 128);
    EXPECT_EQ(mock_meta_->file_size(), static_cast<off_t>(kChunkSize + 128));
  });
}

TEST_F(FileReadWriterTest, AmbiguousTypedFirstAttachmentReconcilesBeforeTruncate) {
  RunInTestFiber([&] {
    std::shared_ptr<swordfs::vfs::FileHandle> handle;
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &handle).ok());
    auto cleanup = folly::makeGuard([&] { (void)handle->Release(); });
    const std::string payload = Repeat('T', 200);
    ASSERT_TRUE(handle->Write(Buf(payload), 0).ok());

    mock_meta_->attach_prepared_commit_on_error = true;
    mock_meta_->attach_prepared_status = Status::OutcomeUnknown("response lost after committed attachment");
    // Typed first-attachment reconciliation probes the durable ChunkID and
    // recognizes that the apparently ambiguous attempt actually committed.
    ASSERT_TRUE(handle->Flush().ok());
    ASSERT_EQ(mock_data_->put_calls, 1);

    struct stat attr{};
    attr.st_size = 64;
    ASSERT_TRUE(
        swordfs::vfs::VfsImpl::SetAttr(kIno, &attr, static_cast<int>(SetAttrField::kSize), std::nullopt, nullptr).ok());

    mock_meta_->attach_prepared_commit_on_error = false;
    mock_meta_->attach_prepared_status = Status::OK();
    ASSERT_TRUE(handle->Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 1);
    EXPECT_EQ(mock_meta_->file_size(), 64);

    swordfs::metadata::FileChunkSnapshot file;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &file).ok());
    ASSERT_TRUE(file.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead head;
    ASSERT_TRUE(cow_metadata_->GetHead(*file.chunk_id, &head).ok());
    EXPECT_EQ(head.size, 64U);

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(128);
    ASSERT_TRUE(reopened.Read(128, 0, out.get()).ok());
    const std::string expected = Repeat('T', 64);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);

    // A later grow must expose zero-filled bytes, never the discarded suffix.
    attr.st_size = 200;
    ASSERT_TRUE(
        swordfs::vfs::VfsImpl::SetAttr(kIno, &attr, static_cast<int>(SetAttrField::kSize), std::nullopt, nullptr).ok());
    FileReadWriter grown(kIno, kMaxParallelFlushes);
    auto grown_data = folly::IOBuf::create(200);
    ASSERT_TRUE(grown.Read(200, 0, grown_data.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(grown_data->data()), grown_data->length()),
              expected + std::string(136, '\0'));

    const auto close_status = handle->Release();
    cleanup.dismiss();
    ASSERT_TRUE(close_status.ok());
  });
}

TEST_F(FileReadWriterTest, WriteAfterPartialTruncateZeroFillsDiscardedRange) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('T', 200)), 0).ok());

    SwordFsAttr attr{};
    attr.size = 64;
    ASSERT_TRUE(rw.SetAttr(attr, SetAttrField::kSize, nullptr).ok());
    ASSERT_TRUE(rw.Write(Buf("Z"), 100).ok());

    auto out = folly::IOBuf::create(101);
    ASSERT_TRUE(rw.Read(101, 0, out.get()).ok());
    std::string expected = Repeat('T', 64) + std::string(36, '\0') + "Z";
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);
  });
}

// ────────────────────────────────────────────────────────────────
// Shared FileReadWriter across file handles
// ────────────────────────────────────────────────────────────────
//
// HandleManager ensures that two open() calls for the same inode share a
// single FileReadWriter instance. These tests verify that
// writes through one handle are visible when reading through another.

TEST_F(FileReadWriterTest, UnflushedWriteVisibleAcrossHandles) {
  RunInTestFiber([&] {
    uint64_t fh1 = 0, fh2 = 0;
    auto &mgr = swordfs::vfs::HandleManager::Instance();
    std::shared_ptr<swordfs::vfs::FileHandle> opened1, opened2;
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened1).ok());
    auto cleanup1 = folly::makeGuard([&] { (void)opened1->Release(); });
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened2).ok());
    auto cleanup2 = folly::makeGuard([&] { (void)opened2->Release(); });
    fh1 = opened1->fh();
    fh2 = opened2->fh();

    auto h1 = mgr.FindAs<swordfs::vfs::FileHandle>(fh1);
    auto h2 = mgr.FindAs<swordfs::vfs::FileHandle>(fh2);
    ASSERT_NE(h1, nullptr);
    ASSERT_NE(h2, nullptr);

    // Write through handle 1, read through handle 2. Shared inode state is
    // verified through the observable data path rather than an internal pointer.
    ASSERT_TRUE(h1->Write(Buf(Repeat('Z', 300)), 100).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(h2->Read(300, 100, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('Z', 300));

    const auto first_close = h1->Release();
    cleanup1.dismiss();
    const auto second_close = h2->Release();
    cleanup2.dismiss();
    ASSERT_TRUE(first_close.ok());
    ASSERT_TRUE(second_close.ok());
  });
}

// ────────────────────────────────────────────────────────────────
// Flushed data visible across handles (same FileReadWriter)
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, FlushedDataVisibleAcrossHandles) {
  RunInTestFiber([&] {
    uint64_t fh1 = 0, fh2 = 0;
    auto &mgr = swordfs::vfs::HandleManager::Instance();
    std::shared_ptr<swordfs::vfs::FileHandle> opened1, opened2;
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened1).ok());
    auto cleanup1 = folly::makeGuard([&] { (void)opened1->Release(); });
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened2).ok());
    auto cleanup2 = folly::makeGuard([&] { (void)opened2->Release(); });
    fh1 = opened1->fh();
    fh2 = opened2->fh();

    auto h1 = mgr.FindAs<swordfs::vfs::FileHandle>(fh1);
    auto h2 = mgr.FindAs<swordfs::vfs::FileHandle>(fh2);
    ASSERT_NE(h1, nullptr);
    ASSERT_NE(h2, nullptr);

    // Write + flush through handle 1.
    ASSERT_TRUE(h1->Write(Buf(Repeat('X', 500)), 0).ok());
    ASSERT_TRUE(h1->Flush().ok());

    // Read through handle 2 — same instance, flushed chunk in map.
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(h2->Read(500, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('X', 500));

    const auto first_close = h1->Release();
    cleanup1.dismiss();
    const auto second_close = h2->Release();
    cleanup2.dismiss();
    ASSERT_TRUE(first_close.ok());
    ASSERT_TRUE(second_close.ok());
  });
}

TEST_F(FileReadWriterTest, WriteAfterFlushOverwritesExistingChunkWithoutLosingUntouchedBytes) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reopened.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
    EXPECT_EQ(mock_meta_->file_size(), 11);
  });
}

TEST_F(FileReadWriterTest, ReopenedWriterAppendsWithinExistingFlushedChunk) {
  RunInTestFiber([&] {
    auto initial = Make();
    ASSERT_TRUE(initial.Write(Buf("hello"), 0).ok());
    ASSERT_TRUE(initial.Flush().ok());

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(reopened.Write(Buf(" world"), 5).ok());
    ASSERT_TRUE(reopened.Flush().ok());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "hello world");
    EXPECT_EQ(mock_meta_->file_size(), 11);
  });
}

TEST_F(FileReadWriterTest, TypedRewriteAdvancesPrivateHeadAndRetainsImmutableObjectVersions) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    swordfs::metadata::FileChunkSnapshot first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &first).ok());
    ASSERT_TRUE(first.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead initial_head;
    ASSERT_TRUE(cow_metadata_->GetHead(*first.chunk_id, &initial_head).ok());

    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    swordfs::metadata::FileChunkSnapshot current;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &current).ok());
    EXPECT_EQ(current.chunk_id, first.chunk_id) << "an attached rewrite must not replace the file mapping";
    swordfs::metadata::cow::COWChunkHead current_head;
    ASSERT_TRUE(cow_metadata_->GetHead(*first.chunk_id, &current_head).ok());
    EXPECT_GT(current_head.revision.Value(), initial_head.revision.Value());
    EXPECT_EQ(current_head.size, 11U);
    const swordfs::chunk::cow::COWObjectKey first_key(*first.chunk_id, initial_head.revision);
    const swordfs::chunk::cow::COWObjectKey replacement_key(*first.chunk_id, current_head.revision);
    EXPECT_EQ(mock_data_->StoredKeys(),
              (std::vector<std::string>{std::string(static_cast<std::string_view>(first_key)),
                                        std::string(static_cast<std::string_view>(replacement_key))}));
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, RewritePutFailureKeepsOldVersionAuthoritativeAndCanRetry) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    swordfs::metadata::FileChunkSnapshot first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &first).ok());
    ASSERT_TRUE(first.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead first_head;
    ASSERT_TRUE(cow_metadata_->GetHead(*first.chunk_id, &first_head).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_data_->put_status = Status::IOError("rewrite put failed");
    EXPECT_TRUE(rw.Flush().ToErrno() == EIO);

    swordfs::metadata::FileChunkSnapshot still_first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &still_first).ok());
    EXPECT_EQ(still_first.chunk_id, first.chunk_id);
    swordfs::metadata::cow::COWChunkHead unchanged;
    ASSERT_TRUE(cow_metadata_->GetHead(*first.chunk_id, &unchanged).ok());
    EXPECT_EQ(unchanged, first_head);
    FileReadWriter old_reader(kIno, kMaxParallelFlushes);
    auto old_out = folly::IOBuf::create(11);
    ASSERT_TRUE(old_reader.Read(11, 0, old_out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(old_out->data()), old_out->length()), "hello world");

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    EXPECT_EQ(published.chunk_id, first.chunk_id)
        << "an attached rewrite advances the private COW head, not the FileMetadata attachment";
    swordfs::metadata::cow::COWChunkHead new_head;
    ASSERT_TRUE(cow_metadata_->GetHead(*published.chunk_id, &new_head).ok());
    EXPECT_GT(new_head.revision.Value(), first_head.revision.Value());
    FileReadWriter new_reader(kIno, kMaxParallelFlushes);
    auto new_out = folly::IOBuf::create(11);
    ASSERT_TRUE(new_reader.Read(11, 0, new_out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(new_out->data()), new_out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, TypedRewriteAfterAmbiguousFinalizationPublishesFreshPrivateRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_EQ(mock_data_->put_calls, 1);
    swordfs::metadata::FileChunkSnapshot before;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &before).ok());
    ASSERT_TRUE(before.chunk_id.has_value());

    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    mock_meta_->finalize_attached_commit_on_error = true;
    mock_meta_->finalize_attached_status = Status::OutcomeUnknown("typed finalization reply lost after commit");
    EXPECT_TRUE(rw.Flush().IsOutcomeUnknown());
    ASSERT_EQ(mock_data_->put_calls, 2);
    swordfs::metadata::cow::COWChunkHead ambiguous_head;
    ASSERT_TRUE(cow_metadata_->GetHead(*before.chunk_id, &ambiguous_head).ok());

    mock_meta_->finalize_attached_commit_on_error = false;
    mock_meta_->finalize_attached_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 3);
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    EXPECT_EQ(published.chunk_id, before.chunk_id);
    swordfs::metadata::cow::COWChunkHead latest_head;
    ASSERT_TRUE(cow_metadata_->GetHead(*before.chunk_id, &latest_head).ok());
    EXPECT_GT(latest_head.revision.Value(), ambiguous_head.revision.Value());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, AmbiguousRewriteRetryFailureKeepsLatestDataWritable) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_commit_on_error = true;
    mock_meta_->finalize_attached_status = Status::OutcomeUnknown("private head committed; final reply lost");
    ASSERT_TRUE(rw.Flush().IsOutcomeUnknown());

    mock_meta_->finalize_attached_commit_on_error = false;
    mock_meta_->finalize_attached_status = Status::IOError("fresh finalization retry failed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    mock_meta_->finalize_attached_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 4);

    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    EXPECT_EQ(published.inode.attr.size, 12U);

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(12);
    ASSERT_TRUE(reader.Read(12, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world!");
  });
}

TEST_F(FileReadWriterTest, TypedRewriteRetryFinalizesEofAfterSuccessfulPrivateHeadCas) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_EQ(mock_meta_->file_size(), 5);
    ASSERT_EQ(mock_data_->put_calls, 1);

    ASSERT_TRUE(rw.Write(Buf(" world"), 5).ok());
    swordfs::metadata::FileChunkSnapshot before;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &before).ok());
    ASSERT_TRUE(before.chunk_id.has_value());
    mock_meta_->finalize_attached_status = Status::IOError("EOF finalization failed after private head CAS");
    EXPECT_EQ(rw.Flush().ToErrno(), EIO);
    EXPECT_EQ(mock_meta_->file_size(), 5);
    ASSERT_EQ(mock_data_->put_calls, 2);

    swordfs::metadata::cow::COWChunkHead privately_published;
    ASSERT_TRUE(cow_metadata_->GetHead(*before.chunk_id, &privately_published).ok());
    EXPECT_EQ(privately_published.size, 11U);

    mock_meta_->finalize_attached_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 3);
    EXPECT_EQ(mock_meta_->file_size(), 11);
    swordfs::metadata::FileChunkSnapshot after;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &after).ok());
    EXPECT_EQ(after.chunk_id, before.chunk_id);
    swordfs::metadata::cow::COWChunkHead finalized_head;
    ASSERT_TRUE(cow_metadata_->GetHead(*before.chunk_id, &finalized_head).ok());
    EXPECT_GT(finalized_head.revision.Value(), privately_published.revision.Value());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "hello world");
  });
}

TEST_F(FileReadWriterTest, TypedRewriteRejectsAnExternallyAdvancedPrivateHeadWithoutDeletingIt) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot attached;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &attached).ok());
    ASSERT_TRUE(attached.chunk_id.has_value());
    swordfs::metadata::cow::COWChunkHead initial;
    ASSERT_TRUE(cow_metadata_->GetHead(*attached.chunk_id, &initial).ok());
    ASSERT_TRUE(rw.Write(Buf(" world"), 5).ok());

    swordfs::metadata::cow::COWChunkRevision external_revision;
    ASSERT_TRUE(cow_metadata_->AllocateRevision(*attached.chunk_id, &external_revision).ok());
    const swordfs::metadata::cow::COWChunkHead external{.revision = external_revision, .size = 5};
    ASSERT_TRUE(cow_metadata_->CompareExchangeHead(*attached.chunk_id, initial, external).ok());

    EXPECT_EQ(rw.Flush().ToErrno(), EEXIST)
        << "an externally advanced private head must not be silently rebased to the old session";
    swordfs::metadata::FileChunkSnapshot after;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &after).ok());
    EXPECT_EQ(after.chunk_id, attached.chunk_id);
    swordfs::metadata::cow::COWChunkHead current;
    ASSERT_TRUE(cow_metadata_->GetHead(*attached.chunk_id, &current).ok());
    EXPECT_EQ(current, external);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
  });
}

TEST_F(FileReadWriterTest, RewriteHydrationPropagatesBackendReadFailure) {
  RunInTestFiber([&] {
    ASSERT_TRUE(SeedPersistedChunk(0, "hello world", 11).ok());

    mock_data_->get_status = Status::IOError("hydrate read failed");
    auto rw = Make(11);
    const auto status = rw.Write(Buf("H"), 0);
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "hydrate read failed");
  });
}

TEST_F(FileReadWriterTest, RewriteHydrationRejectsObjectShorterThanDescriptor) {
  RunInTestFiber([&] {
    ASSERT_TRUE(SeedPersistedChunk(0, "short", 11).ok());

    auto rw = Make(11);
    const auto status = rw.Write(Buf("H"), 0);
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_NE(status.message().find("shorter than metadata descriptor"), std::string::npos);
  });
}

TEST_F(FileReadWriterTest, RewriteHydrationRejectsDescriptorLargerThanChunk) {
  RunInTestFiber([&] {
    // The mechanism's private head is corrupt, even though the logical
    // FileMetadata attachment identity itself remains well-formed.
    std::string key;
    ASSERT_TRUE(mock_meta_
                    ->SeedTypedChunkForTest(kIno, 0, kChunkSize + 1, &key,
                                            /*allow_malformed_size=*/true)
                    .ok());
    auto rw = Make(kChunkSize + 1);
    const auto status = rw.Write(Buf("H"), 0);
    EXPECT_TRUE(status.ToErrno() == EIO);
  });
}

TEST_F(FileReadWriterTest, RewriteHydratesZeroLengthPublishedChunkWithoutBackendRead) {
  RunInTestFiber([&] {
    std::string key;
    ASSERT_TRUE(mock_meta_->SeedTypedChunkForTest(kIno, 0, 0, &key).ok());
    mock_data_->get_status = Status::IOError("zero-length head must not require data hydration");

    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("new"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    mock_data_->get_status = Status::OK();
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(3);
    ASSERT_TRUE(reader.Read(3, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "new");
  });
}

TEST_F(FileReadWriterTest, WriteAfterFailedInitialCommitPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    mock_meta_->attach_prepared_status = Status::IOError("typed first attachment failed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);

    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    mock_meta_->attach_prepared_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot file;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &file).ok());
    ASSERT_TRUE(file.chunk_id.has_value());
    EXPECT_EQ(file.inode.attr.size, 6U);

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reopened.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
  });
}

TEST_F(FileReadWriterTest, WriteAfterAmbiguousTypedAttachmentPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());

    mock_meta_->attach_prepared_commit_on_error = true;
    mock_meta_->attach_prepared_status = Status::OutcomeUnknown("response lost after committed attachment");
    // Authority reconciliation confirms that the typed first attachment
    // committed, despite the backend returning an ambiguous response.
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot committed;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &committed).ok());
    ASSERT_TRUE(committed.chunk_id.has_value());

    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    mock_meta_->attach_prepared_commit_on_error = false;
    mock_meta_->attach_prepared_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot current;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &current).ok());
    EXPECT_EQ(current.chunk_id, committed.chunk_id);
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reopened.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
  });
}

TEST_F(FileReadWriterTest, WriteAfterFailedRewriteCommitPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_status = Status::IOError("typed rewrite finalization failed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    mock_meta_->finalize_attached_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(12);
    ASSERT_TRUE(reopened.Read(12, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world!");
  });
}

TEST_F(FileReadWriterTest, WriteAfterAmbiguousRewriteCommitPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_commit_on_error = true;
    mock_meta_->finalize_attached_status = Status::OutcomeUnknown("typed finalization reply lost after commit");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);

    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    mock_meta_->finalize_attached_commit_on_error = false;
    mock_meta_->finalize_attached_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(12);
    ASSERT_TRUE(reopened.Read(12, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world!");
  });
}

TEST_F(FileReadWriterTest, RewriteRetryPropagatesMetadataLookupFailure) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_status = Status::IOError("typed rewrite finalization failed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);

    mock_meta_->finalize_attached_status = Status::OK();
    mock_meta_->find_chunk_status = Status::IOError("retry lookup failed");
    const auto status = rw.Flush();
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "retry lookup failed");

    mock_meta_->find_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot published;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &published).ok());
    ASSERT_TRUE(published.chunk_id.has_value());
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, TypedRewriteRetryPreservesConcurrentFileEofGrowth) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    swordfs::metadata::FileChunkSnapshot first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &first).ok());
    ASSERT_TRUE(first.chunk_id.has_value());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    mock_meta_->finalize_attached_status = Status::IOError("typed EOF finalization failed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    // Simulate another chunk of the same inode growing EOF while this writer
    // has a dirty local generation. Finalization must not shrink that EOF.
    mock_meta_->set_file_size(128);
    mock_meta_->finalize_attached_status = Status::OK();

    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot current;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &current).ok());
    EXPECT_EQ(current.chunk_id, first.chunk_id);
    EXPECT_EQ(current.inode.attr.size, 128U);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(12);
    ASSERT_TRUE(reopened.Read(12, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world!");
  });
}

TEST_F(FileReadWriterTest, DetachedTypedAttachmentCannotBeReusedByStaleDirtySession) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &first).ok());
    ASSERT_TRUE(first.chunk_id.has_value());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_status = Status::IOError("typed finalization unavailable");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    mock_meta_->DetachTypedChunkForTest(kIno, 0);
    mock_meta_->finalize_attached_status = Status::OK();
    ASSERT_EQ(rw.Flush().ToErrno(), EEXIST)
        << "detached FileMetadata identity cannot be resurrected by a dirty session";
    EXPECT_TRUE(mock_data_->delete_calls.empty());

    // A newly opened session may materialize the now-empty logical mapping,
    // but it must receive a fresh ChunkID rather than revive the detached ID.
    FileReadWriter recreated(kIno, kMaxParallelFlushes);
    ASSERT_TRUE(recreated.Write(Buf("fresh world"), 0).ok());
    ASSERT_TRUE(recreated.Flush().ok());
    swordfs::metadata::FileChunkSnapshot current;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &current).ok());
    ASSERT_TRUE(current.chunk_id.has_value());
    EXPECT_NE(current.chunk_id, first.chunk_id);
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reopened.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "fresh world");
  });
}

TEST_F(FileReadWriterTest, TypedRewriteRetryAfterFailedPutDoesNotDeleteUnuploadedObject) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot first;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &first).ok());
    ASSERT_TRUE(first.chunk_id.has_value());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_data_->put_status = Status::IOError("rewrite put failed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    ASSERT_EQ(mock_data_->put_calls, 2);
    const auto existing = mock_data_->StoredKeys();
    ASSERT_EQ(existing.size(), 1U)
        << "the failed object Put must not insert an immutable object into the backing store";

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 3);
    const auto stored_keys = mock_data_->StoredKeys();
    EXPECT_EQ(stored_keys.size(), 2U);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    swordfs::metadata::FileChunkSnapshot current;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &current).ok());
    EXPECT_EQ(current.chunk_id, first.chunk_id);
    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, TypedRewriteFinalizationNotFoundNeverDeletesPublishedPrivateHead) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    swordfs::metadata::FileChunkSnapshot before;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &before).ok());
    ASSERT_TRUE(before.chunk_id.has_value());
    mock_meta_->finalize_attached_status = Status::NotFound("typed EOF finalization unavailable");
    const auto status = rw.Flush();
    EXPECT_TRUE(status.IsNotFound());
    swordfs::metadata::FileChunkSnapshot after;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &after).ok());
    EXPECT_EQ(after.chunk_id, before.chunk_id);
    swordfs::metadata::cow::COWChunkHead published;
    ASSERT_TRUE(cow_metadata_->GetHead(*before.chunk_id, &published).ok());
    const swordfs::chunk::cow::COWObjectKey current_key(*before.chunk_id, published.revision);
    const auto keys = mock_data_->StoredKeys();
    EXPECT_NE(std::find(keys.begin(), keys.end(), std::string(static_cast<std::string_view>(current_key))), keys.end());
    EXPECT_TRUE(mock_data_->delete_calls.empty()) << "foreground must not delete an attached private head";
  });
}

TEST_F(FileReadWriterTest, TypedRewriteAfterFailedFinalizationRejectsNewerPrivateHead) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    swordfs::metadata::FileChunkSnapshot file;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &file).ok());
    ASSERT_TRUE(file.chunk_id.has_value());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_status = Status::IOError("EOF finalization failed after private CAS");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    ASSERT_EQ(mock_data_->put_calls, 2);

    swordfs::metadata::cow::COWChunkHead after_failed_finalize;
    ASSERT_TRUE(cow_metadata_->GetHead(*file.chunk_id, &after_failed_finalize).ok());
    swordfs::metadata::cow::COWChunkRevision external_revision;
    ASSERT_TRUE(cow_metadata_->AllocateRevision(*file.chunk_id, &external_revision).ok());
    const swordfs::metadata::cow::COWChunkHead external{.revision = external_revision, .size = 11};
    const swordfs::chunk::cow::COWObjectKey external_key(*file.chunk_id, external_revision);
    ASSERT_TRUE(mock_data_
                    ->Put(std::string(static_cast<std::string_view>(external_key)),
                          std::make_unique<folly::IOBuf>(Buf("external!!!")))
                    .ok());
    ASSERT_TRUE(cow_metadata_->CompareExchangeHead(*file.chunk_id, after_failed_finalize, external).ok());

    mock_meta_->finalize_attached_status = Status::OK();
    EXPECT_EQ(rw.Flush().ToErrno(), EEXIST);
    swordfs::metadata::cow::COWChunkHead still_external;
    ASSERT_TRUE(cow_metadata_->GetHead(*file.chunk_id, &still_external).ok());
    EXPECT_EQ(still_external, external);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reopened.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "external!!!");
  });
}

TEST_F(FileReadWriterTest, TypedRewriteRemainsRetryableAfterTransientMetadataSnapshotFailure) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->finalize_attached_status = Status::IOError("typed finalization failed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    ASSERT_EQ(mock_data_->put_calls, 2);

    mock_meta_->find_chunk_status = Status::NotFound("transient typed snapshot miss");
    mock_meta_->finalize_attached_status = Status::OK();

    ASSERT_TRUE(rw.Flush().IsNotFound());
    mock_meta_->find_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reopened.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, AmbiguousCommittedTypedTruncateReconcilesLocalEofAndKeepsDirtyPrefix) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->truncate_commit_on_error = true;
    mock_meta_->set_truncate_status(Status::OutcomeUnknown("typed shrink committed but response was lost"));
    ASSERT_TRUE(rw.Truncate(5).IsOutcomeUnknown());

    swordfs::metadata::FileMappingSnapshot committed;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &committed).ok());
    ASSERT_EQ(committed.inode.attr.size, 5U);
    ASSERT_EQ(committed.mappings.size(), 1U);

    mock_meta_->truncate_commit_on_error = false;
    mock_meta_->set_truncate_status(Status::OK());
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(5);
    ASSERT_TRUE(reader.Read(5, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO");
  });
}

TEST_F(FileReadWriterTest, AmbiguousCommittedTypedSizeSetAttrReconcilesLocalEofAndKeepsDirtyPrefix) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    SwordFsAttr attr{};
    attr.size = 5;
    mock_meta_->set_attr_commit_on_error = true;
    mock_meta_->set_attr_status = Status::OutcomeUnknown("typed size setattr committed; reply lost");
    ASSERT_TRUE(rw.SetAttr(attr, SetAttrField::kSize, nullptr).IsOutcomeUnknown());

    swordfs::metadata::FileMappingSnapshot committed;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &committed).ok());
    ASSERT_EQ(committed.inode.attr.size, 5U);
    ASSERT_EQ(committed.mappings.size(), 1U);

    mock_meta_->set_attr_commit_on_error = false;
    mock_meta_->set_attr_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(5);
    ASSERT_TRUE(reader.Read(5, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO");
  });
}

TEST_F(FileReadWriterTest, PartialTruncateOfDirtyRewriteClampsExpectedDescriptor) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    ASSERT_TRUE(rw.Truncate(5).ok());
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reader(kIno, kMaxParallelFlushes);
    auto out = folly::IOBuf::create(5);
    ASSERT_TRUE(reader.Read(5, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO");
  });
}

TEST_F(FileReadWriterTest, TruncateAfterAmbiguousInitialPublicationMayLeaveUploadedObject) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    mock_meta_->attach_prepared_status = Status::IOError("typed attachment was not committed");
    ASSERT_EQ(rw.Flush().ToErrno(), EIO);
    ASSERT_EQ(mock_data_->StoredKeys().size(), 1U);
    swordfs::metadata::FileChunkSnapshot missing;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &missing).ok());
    EXPECT_FALSE(missing.chunk_id.has_value());

    mock_meta_->attach_prepared_status = Status::OK();
    ASSERT_TRUE(rw.Truncate(0).ok());
    // The earlier publication result was ambiguous and no authoritative chunk
    // exists to classify this object. Exceptional orphan completeness is not
    // part of truncate correctness, so the object may remain as garbage.
    EXPECT_EQ(mock_data_->StoredKeys().size(), 1U);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
  });
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, TruncateCommitsTypedFileSizeWithoutInventingMappings) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Truncate(1024).ok());
    swordfs::metadata::FileMappingSnapshot file;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &file).ok());
    EXPECT_EQ(file.inode.attr.size, 1024);
    EXPECT_TRUE(file.mappings.empty());
    EXPECT_EQ(mock_meta_->file_size(), 1024);
  });
}

TEST_F(FileReadWriterTest, RetainedTypedChunkCanFlushAfterShrinkClampsItsPrivateHead) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    swordfs::metadata::FileChunkSnapshot before;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &before).ok());
    ASSERT_TRUE(before.chunk_id.has_value());

    ASSERT_TRUE(rw.Truncate(5).ok());
    swordfs::metadata::cow::COWChunkHead clamped;
    ASSERT_TRUE(cow_metadata_->GetHead(*before.chunk_id, &clamped).ok());
    ASSERT_EQ(clamped.size, 5U);

    ASSERT_TRUE(rw.Write(Buf("Y"), 1).ok());
    ASSERT_TRUE(rw.Flush().ok()) << "a legal same-mount rewrite must not conflict with its own boundary clamp";

    swordfs::metadata::FileChunkSnapshot after;
    ASSERT_TRUE(mock_meta_->ReadFileChunkSnapshot(kIno, 0, &after).ok());
    EXPECT_EQ(after.chunk_id, before.chunk_id);
    EXPECT_EQ(after.inode.attr.size, 5U);
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto data = folly::IOBuf::create(5);
    ASSERT_TRUE(reopened.Read(5, 0, data.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(data->data()), data->length()), "hYllo");
  });
}

TEST_F(FileReadWriterTest, TruncateBeyondSupportedFileSizeIsRejectedBeforeMetadataMutation) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    const size_t too_large = static_cast<size_t>(swordfs::metadata::kMaxSupportedFileSize) + 1;

    const auto status = rw.Truncate(too_large);

    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(mock_meta_->truncate_calls, 0);
    EXPECT_EQ(mock_meta_->file_size(), 64);
  });
}

TEST_F(FileReadWriterTest, SetAttrBeyondSupportedFileSizeIsRejectedBeforeMetadataMutation) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    SwordFsAttr attr;
    attr.size = swordfs::metadata::kMaxSupportedFileSize + 1;

    const auto status = rw.SetAttr(attr, SetAttrField::kSize, nullptr);

    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(mock_meta_->file_size(), 64);
  });
}

TEST_F(FileReadWriterTest, TruncatePropagatesMetaError) {
  RunInTestFiber([&] {
    mock_meta_->set_truncate_status(Status::Internal("truncate failed"));
    auto rw = Make();
    auto status = rw.Truncate(1024);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.ToErrno(), EIO);
  });
}

TEST_F(FileReadWriterTest, TypedSizeTransitionPropagatesMappingSnapshotFailure) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    mock_meta_->get_inode_status = Status::IOError("injected mapping snapshot read failure");
    const auto status = rw.Truncate(32);
    EXPECT_EQ(status.ToErrno(), EIO);
    EXPECT_EQ(status.message(), "injected mapping snapshot read failure");
    EXPECT_EQ(mock_meta_->file_size(), 64);
    EXPECT_EQ(mock_meta_->truncate_calls, 0);
  });
}

TEST_F(FileReadWriterTest, TruncateDropsDirtyChunks) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());

    // Truncating to zero must drop the dirty chunk and move logical EOF to
    // zero, so a later read returns no bytes from the discarded data.
    ASSERT_TRUE(rw.Truncate(0).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(16, 0, out.get()).ok());
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, HighIndexCachedTruncateDoesNotAliasLowChunk) {
  RunInTestFiber([&] {
    constexpr ChunkIndex kHighIndex = uint64_t{1} << 32;
    constexpr off_t kHighStart = static_cast<off_t>(kHighIndex * kChunkSize);
    auto rw = Make();

    ASSERT_TRUE(rw.Write(Buf("L"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("H"), kHighStart + 3).ok());

    // Keep only the first two sparse bytes of the high-index chunk. The
    // low-index dirty chunk must remain independent rather than aliasing it.
    ASSERT_TRUE(rw.Truncate(static_cast<size_t>(kHighStart) + 2).ok());

    auto low = folly::IOBuf::create(1);
    ASSERT_TRUE(rw.Read(1, 0, low.get()).ok());
    ASSERT_EQ(low->length(), 1U);
    EXPECT_EQ(low->data()[0], 'L');

    auto high = folly::IOBuf::create(2);
    ASSERT_TRUE(rw.Read(2, kHighStart, high.get()).ok());
    ASSERT_EQ(high->length(), 2U);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(high->data()), high->length()), std::string("\0\0", 2));
  });
}

TEST_F(FileReadWriterTest, TypedTruncateDetachesWholeMappingsWithoutForegroundDelete) {
  RunInTestFiber([&] {
    auto rw = Make(3 * kChunkSize);
    for (ChunkIndex i = 0; i < 3; ++i) {
      ASSERT_TRUE(SeedPersistedChunk(i, Repeat('A', kChunkSize), kChunkSize).ok());
    }
    for (ChunkIndex i = 0; i < 3; ++i) {
      auto out = folly::IOBuf::create(kChunkSize);
      ASSERT_TRUE(rw.Read(kChunkSize, i * kChunkSize, out.get()).ok());
      EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('A', kChunkSize));
    }
    ASSERT_EQ(mock_data_->StoredKeys().size(), 3U);

    swordfs::metadata::FileMappingSnapshot before;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &before).ok());
    ASSERT_EQ(before.mappings.size(), 3U);
    ASSERT_TRUE(rw.Truncate(1).ok());
    swordfs::metadata::FileMappingSnapshot after;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &after).ok());
    EXPECT_EQ(after.inode.attr.size, 1U);
    ASSERT_EQ(after.mappings.size(), 1U);
    EXPECT_EQ(after.mappings.front().index, 0U);
    EXPECT_EQ(after.mappings.front().chunk_id, before.mappings.front().chunk_id);
    swordfs::metadata::cow::COWChunkHead retained;
    ASSERT_TRUE(cow_metadata_->GetHead(after.mappings.front().chunk_id, &retained).ok());
    EXPECT_EQ(retained.size, 1U);
    // Logical detach is authoritative even if best-effort asynchronous
    // cleanup has not yet reclaimed the old immutable objects.
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    EXPECT_EQ(mock_data_->StoredKeys().size(), 3U);
  });
}

TEST_F(FileReadWriterTest, ConcurrentReadsProceedWhileAnotherReadWaitsForBackend) {
  std::string key;
  RunInTestFiber([&] { ASSERT_TRUE(mock_meta_->SeedTypedChunkForTest(kIno, 0, kChunkSize, &key).ok()); });
  auto data = std::make_unique<folly::IOBuf>(Buf(Repeat('R', kChunkSize)));
  ASSERT_TRUE(mock_data_->Put(key, std::move(data)).ok());

  auto rw = Make(kChunkSize);
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton first_get_started;
  folly::fibers::Baton release_first_get;
  folly::fibers::Baton second_get_started;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;
  mock_data_->BlockNextGet(&first_get_started, &release_first_get, &second_get_started);

  Status first_status;
  Status second_status;
  auto first_out = folly::IOBuf::create(kChunkSize);
  auto second_out = folly::IOBuf::create(kChunkSize);
  fm.addTask([&] {
    first_status = rw.Read(kChunkSize, 0, first_out.get());
    first_done.post();
  });
  fm.addTask([&] {
    first_get_started.wait();
    second_status = rw.Read(kChunkSize, 0, second_out.get());
    second_done.post();
  });

  const bool second_read_completed =
      swordfs::test::DriveEventBaseUntil(evb, [&] { return second_get_started.try_wait() && second_done.try_wait(); });
  EXPECT_TRUE(second_read_completed) << "independent read must complete while the first chunk Get is blocked";
  if (!second_read_completed) {
    release_first_get.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return first_done.try_wait() && second_done.try_wait(); },
        "FileReadWriter independent-read timeout cleanup");
    return;
  }
  EXPECT_TRUE(second_status.ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(second_out->data()), second_out->length()),
            Repeat('R', kChunkSize));

  release_first_get.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_done.try_wait(); }, "FileReadWriter first read completion after Get release");
  EXPECT_TRUE(first_status.ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first_out->data()), first_out->length()),
            Repeat('R', kChunkSize));
}

TEST_F(FileReadWriterTest, IndependentChunkOverwriteHydrationDoesNotSerializeOnInode) {
  for (ChunkIndex index = 0; index < 2; ++index) {
    std::string key;
    RunInTestFiber([&] { ASSERT_TRUE(mock_meta_->SeedTypedChunkForTest(kIno, index, kChunkSize, &key).ok()); });
    auto data = std::make_unique<folly::IOBuf>(Buf(Repeat(index == 0 ? 'A' : 'B', kChunkSize)));
    ASSERT_TRUE(mock_data_->Put(key, std::move(data)).ok());
  }

  auto rw = Make(2 * kChunkSize);
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton first_get_started;
  folly::fibers::Baton release_first_get;
  folly::fibers::Baton second_get_started;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;
  mock_data_->BlockNextGet(&first_get_started, &release_first_get, &second_get_started);

  Status first_status;
  Status second_status;
  fm.addTask([&] {
    first_status = rw.Write(Buf("X"), 0);
    first_done.post();
  });
  fm.addTask([&] {
    first_get_started.wait();
    second_status = rw.Write(Buf("Y"), kChunkSize);
    second_done.post();
  });

  const bool second_write_completed =
      swordfs::test::DriveEventBaseUntil(evb, [&] { return second_get_started.try_wait() && second_done.try_wait(); });
  EXPECT_TRUE(second_write_completed) << "independent overwrite hydration must complete while the first Get is blocked";
  if (!second_write_completed) {
    release_first_get.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return first_done.try_wait() && second_done.try_wait(); },
        "FileReadWriter independent-overwrite timeout cleanup");
    return;
  }
  EXPECT_TRUE(second_status.ok());

  release_first_get.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_done.try_wait(); }, "FileReadWriter first overwrite completion after Get release");
  EXPECT_TRUE(first_status.ok());
}

TEST_F(FileReadWriterTest, WriteDuringBlockedFlushDoesNotWaitForRemotePut) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton write_started;
  folly::fibers::Baton write_done;
  folly::fibers::Baton probe_done;
  folly::fibers::Baton flush_done;
  mock_data_->BlockNextPut(&put_started, &release_put);

  Status flush_status;
  Status write_status;
  Status get_attr_status;
  struct stat blocked_flush_attr{};
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    SwordFsInode inode;
    get_attr_status = rw.GetAttr(&inode);
    if (get_attr_status.ok()) {
      inode.attr.ToPosixStat(&blocked_flush_attr);
    }
    write_started.post();
    write_status = rw.Write(Buf("BBBB"), 4);
    write_done.post();
  });
  fm.addTask([&] {
    write_started.wait();
    for (int i = 0; i < 4; ++i) {
      folly::fibers::yield();
    }
    probe_done.post();
  });

  const bool probe_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return probe_done.try_wait(); });
  EXPECT_TRUE(probe_completed) << "write-progress probe must complete while the older Put is blocked";
  if (!probe_completed) {
    release_put.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && write_done.try_wait(); },
        "FileReadWriter blocked-Put timeout cleanup");
    return;
  }
  EXPECT_TRUE(write_done.try_wait()) << "foreground write waited for the older generation's remote Put";
  ASSERT_TRUE(get_attr_status.ok()) << get_attr_status.message();
  EXPECT_EQ(blocked_flush_attr.st_size, 4);
  EXPECT_EQ(blocked_flush_attr.st_blocks, 1);

  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && write_done.try_wait(); },
      "FileReadWriter blocked-Put completion after release");
  ASSERT_TRUE(flush_status.ok());
  ASSERT_TRUE(write_status.ok());

  RunInTestFiber([&] {
    SwordFsInode inode;
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    EXPECT_EQ(inode.attr.size, 8U) << "the older flush must not clear the size overlay for the newer local generation";

    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(rw.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "AAAABBBB");

    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
}

TEST_F(FileReadWriterTest, WriteDuringBlockedCommitDoesNotWaitForRemotePublication) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton commit_started;
  folly::fibers::Baton release_commit;
  folly::fibers::Baton write_started;
  folly::fibers::Baton write_done;
  folly::fibers::Baton probe_done;
  folly::fibers::Baton flush_done;
  mock_meta_->BlockNextCommit(&commit_started, &release_commit);

  Status flush_status;
  Status write_status;
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    commit_started.wait();
    write_started.post();
    write_status = rw.Write(Buf("BBBB"), 4);
    write_done.post();
  });
  fm.addTask([&] {
    write_started.wait();
    for (int i = 0; i < 4; ++i) {
      folly::fibers::yield();
    }
    probe_done.post();
  });

  const bool probe_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return probe_done.try_wait(); });
  EXPECT_TRUE(probe_completed) << "write-progress probe must complete while the older commit is blocked";
  if (!probe_completed) {
    release_commit.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && write_done.try_wait(); },
        "FileReadWriter blocked-commit timeout cleanup");
    return;
  }
  EXPECT_TRUE(write_done.try_wait()) << "foreground write waited for the older generation's metadata commit";

  release_commit.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && write_done.try_wait(); },
      "FileReadWriter blocked-commit completion after release");
  ASSERT_TRUE(flush_status.ok());
  ASSERT_TRUE(write_status.ok());

  RunInTestFiber([&] {
    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(rw.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "AAAABBBB");

    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
}

TEST_F(FileReadWriterTest, FailedOlderPutPreservesRepeatedWritesDuringFlush) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton writes_done;
  folly::fibers::Baton flush_done;
  mock_data_->put_status = Status::IOError("injected old-generation put failure");
  mock_data_->BlockNextPut(&put_started, &release_put);

  Status flush_status;
  Status first_write_status;
  Status second_write_status;
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    first_write_status = rw.Write(Buf("BBBB"), 4);
    second_write_status = rw.Write(Buf("CC"), 1);
    writes_done.post();
  });

  const bool writes_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return writes_done.try_wait(); });
  EXPECT_TRUE(writes_completed) << "new-generation writes must complete while the old Put is blocked";
  if (!writes_completed) {
    release_put.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && writes_done.try_wait(); },
        "FileReadWriter failed-Put timeout cleanup");
    return;
  }
  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait(); }, "FileReadWriter failed-Put flush completion after release");
  ASSERT_TRUE(first_write_status.ok());
  ASSERT_TRUE(second_write_status.ok());
  EXPECT_FALSE(flush_status.ok());

  RunInTestFiber([&] {
    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(rw.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "ACCABBBB");

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "ACCABBBB");
  });
}

TEST_F(FileReadWriterTest, WriteDuringAmbiguousTypedAttachmentPreservesLatestLocalGeneration) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton commit_started;
  folly::fibers::Baton release_commit;
  folly::fibers::Baton write_done;
  folly::fibers::Baton flush_done;
  mock_meta_->attach_prepared_status = Status::OutcomeUnknown("typed attachment response lost after commit");
  mock_meta_->attach_prepared_commit_on_error = true;
  mock_meta_->BlockNextCommit(&commit_started, &release_commit);

  Status flush_status;
  Status write_status;
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    commit_started.wait();
    write_status = rw.Write(Buf("BBBB"), 4);
    write_done.post();
  });

  const bool write_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return write_done.try_wait(); });
  EXPECT_TRUE(write_completed) << "new-generation write must complete while the ambiguous commit is blocked";
  if (!write_completed) {
    release_commit.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && write_done.try_wait(); },
        "FileReadWriter ambiguous-commit timeout cleanup");
    return;
  }
  release_commit.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait(); }, "FileReadWriter ambiguous-commit flush completion after release");
  ASSERT_TRUE(write_status.ok());
  // The private object was uploaded and typed FileMetadata committed. Its
  // post-error authoritative readback resolves the ambiguous response.
  EXPECT_TRUE(flush_status.ok());

  RunInTestFiber([&] {
    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(rw.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "AAAABBBB");

    mock_meta_->attach_prepared_status = Status::OK();
    mock_meta_->attach_prepared_commit_on_error = false;
    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
}

TEST_F(FileReadWriterTest, ConcurrentFlushBarriersPublishGenerationsInOrder) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton first_put_started;
  folly::fibers::Baton release_first_put;
  folly::fibers::Baton write_done;
  folly::fibers::Baton second_flush_started;
  folly::fibers::Baton probe_done;
  folly::fibers::Baton first_flush_done;
  folly::fibers::Baton second_flush_done;
  mock_data_->BlockNextPut(&first_put_started, &release_first_put);

  Status first_flush_status;
  Status second_flush_status;
  Status write_status;
  fm.addTask([&] {
    first_flush_status = rw.Flush();
    first_flush_done.post();
  });
  fm.addTask([&] {
    first_put_started.wait();
    write_status = rw.Write(Buf("BBBB"), 4);
    write_done.post();
  });
  fm.addTask([&] {
    write_done.wait();
    second_flush_started.post();
    second_flush_status = rw.Flush();
    second_flush_done.post();
  });
  fm.addTask([&] {
    second_flush_started.wait();
    for (int i = 0; i < 4; ++i) {
      folly::fibers::yield();
    }
    probe_done.post();
  });

  const bool probe_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return probe_done.try_wait(); });
  EXPECT_TRUE(probe_completed) << "second-flush probe must run while the first Put is blocked";
  if (!probe_completed) {
    release_first_put.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return first_flush_done.try_wait() && second_flush_done.try_wait(); },
        "FileReadWriter ordered-flush timeout cleanup");
    return;
  }
  EXPECT_EQ(mock_data_->put_calls, 1) << "a newer generation began publication before the older generation completed";

  release_first_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return first_flush_done.try_wait() && second_flush_done.try_wait(); },
      "FileReadWriter ordered flush completion after release");
  ASSERT_TRUE(write_status.ok());
  EXPECT_TRUE(first_flush_status.ok());
  EXPECT_TRUE(second_flush_status.ok());
  EXPECT_EQ(mock_data_->put_calls, 2);

  RunInTestFiber([&] {
    FileReadWriter reopened(kIno, kMaxParallelFlushes);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
}

TEST_F(FileReadWriterTest, MultiChunkFlushStartsAnotherPutWhileFirstPutIsBlocked) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize) + Repeat('B', kChunkSize)), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton first_put_started;
  folly::fibers::Baton release_first_put;
  folly::fibers::Baton second_put_started;
  folly::fibers::Baton probe_done;
  folly::fibers::Baton flush_done;
  mock_data_->BlockNextPut(&first_put_started, &release_first_put, &second_put_started);

  Status flush_status;
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    first_put_started.wait();
    for (int i = 0; i < 4; ++i) {
      folly::fibers::yield();
    }
    probe_done.post();
  });

  const bool probe_completed = swordfs::test::DriveEventBaseUntil(evb, [&] { return probe_done.try_wait(); });
  EXPECT_TRUE(probe_completed) << "multi-chunk flush probe must run while the first Put is blocked";
  if (!probe_completed) {
    release_first_put.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait(); }, "FileReadWriter multi-chunk timeout cleanup");
    return;
  }
  EXPECT_TRUE(second_put_started.try_wait()) << "one chunk's remote Put serialized every chunk in the inode";

  release_first_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait(); }, "FileReadWriter multi-chunk flush completion after release");
  EXPECT_TRUE(flush_status.ok());
  // The two independent Put calls must overlap, but an initial attachment
  // can lose the FileMetadata EOF precondition to the other chunk and retry
  // under a *fresh* ChunkID. Do not mistake that safe retry for serialization.
  EXPECT_GE(mock_data_->put_calls, 2);
  RunInTestFiber([&] {
    swordfs::metadata::FileMappingSnapshot snapshot;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &snapshot).ok());
    EXPECT_EQ(snapshot.mappings.size(), 2U);
    EXPECT_EQ(snapshot.inode.attr.size, kChunkSize * 2);
  });
}

TEST_F(FileReadWriterTest, TruncateWaitsForBlockedFlushPublication) {
  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton put_started;
  folly::fibers::Baton release_put;
  folly::fibers::Baton truncate_started;
  folly::fibers::Baton flush_done;
  folly::fibers::Baton truncate_done;
  mock_data_->BlockNextPut(&put_started, &release_put);

  Status flush_status;
  Status truncate_status;
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
    truncate_started.post();
    truncate_status = rw.Truncate(0);
    truncate_done.post();
  });

  const bool truncate_blocked =
      swordfs::test::DriveEventBaseUntil(evb, [&] { return put_started.try_wait() && truncate_started.try_wait(); });
  EXPECT_TRUE(truncate_blocked) << "truncate must reach the publication barrier within the test watchdog";
  if (!truncate_blocked) {
    release_put.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return flush_done.try_wait() && truncate_done.try_wait(); },
        "FileReadWriter truncate-vs-flush timeout cleanup");
    return;
  }
  EXPECT_EQ(mock_meta_->truncate_calls, 0) << "truncate raced an older publication and could be republished over EOF";

  release_put.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return flush_done.try_wait() && truncate_done.try_wait(); },
      "FileReadWriter truncate-vs-flush completion after release");
  EXPECT_TRUE(flush_status.ok());
  EXPECT_TRUE(truncate_status.ok());
  RunInTestFiber([&] {
    swordfs::metadata::FileMappingSnapshot snapshot;
    ASSERT_TRUE(mock_meta_->ReadFileMappingSnapshot(kIno, &snapshot).ok());
    EXPECT_EQ(snapshot.inode.attr.size, 0U);
    EXPECT_TRUE(snapshot.mappings.empty());
  });
}

TEST_F(FileReadWriterTest, TruncateWaitsForBlockedReadWithoutBlockingEventBase) {
  RunInTestFiber([&] { ASSERT_TRUE(SeedPersistedChunk(0, Repeat('R', kChunkSize), kChunkSize).ok()); });

  auto rw = Make(kChunkSize);
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton get_started;
  folly::fibers::Baton release_get;
  folly::fibers::Baton truncate_started;
  folly::fibers::Baton read_done;
  folly::fibers::Baton truncate_done;
  mock_data_->BlockNextGet(&get_started, &release_get);

  Status read_status;
  Status truncate_status;
  auto out = folly::IOBuf::create(kChunkSize);
  fm.addTask([&] {
    read_status = rw.Read(kChunkSize, 0, out.get());
    read_done.post();
  });
  fm.addTask([&] {
    get_started.wait();
    truncate_started.post();
    truncate_status = rw.Truncate(0);
    truncate_done.post();
  });

  const bool truncate_blocked =
      swordfs::test::DriveEventBaseUntil(evb, [&] { return get_started.try_wait() && truncate_started.try_wait(); });
  EXPECT_TRUE(truncate_blocked) << "truncate must reach the read barrier within the test watchdog";
  if (!truncate_blocked) {
    release_get.post();
    swordfs::test::DriveEventBaseUntilOrAbort(
        evb, [&] { return read_done.try_wait() && truncate_done.try_wait(); },
        "FileReadWriter truncate-vs-read timeout cleanup");
    return;
  }
  EXPECT_EQ(mock_meta_->file_size(), static_cast<off_t>(kChunkSize));

  release_get.post();
  swordfs::test::DriveEventBaseUntilOrAbort(
      evb, [&] { return read_done.try_wait() && truncate_done.try_wait(); },
      "FileReadWriter truncate-vs-read completion after release");

  EXPECT_TRUE(read_status.ok());
  EXPECT_TRUE(truncate_status.ok());
  EXPECT_EQ(mock_meta_->file_size(), 0);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('R', kChunkSize));
}
