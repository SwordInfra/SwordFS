// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

// Test-only logical Chunk runtime and backend doubles for Slice.
// Keep mechanism-specific ChunkMetadata and factory selection in their own fixtures.

#include <folly/fibers/Baton.h>
#include <folly/io/IOBuf.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"

namespace swordfs::test::chunk_test {

using metadata::ChunkIndex;
using metadata::IMetaEngine;
using metadata::InodeID;
using metadata::Limits;
using metadata::RenameFlag;
using metadata::SetAttrField;
using metadata::SwordFsAttr;
using metadata::SwordFsInode;
using metadata::SwordFsStatFs;
using metadata::SwordFsVolume;
using storage::IDataEngine;
using utils::Status;

constexpr uint64_t kChunkTestSize = 1024;

inline auto Buf(const std::string &value) {
  return *folly::IOBuf::copyBuffer(value.data(), value.size());
}

// Minimal FileMetadata test double shared by Slice fixtures.
class MissingMetaEngine : public IMetaEngine {
 public:
  void ConfigureChunkSize(uint64_t chunk_size) {
    chunk_size_ = chunk_size;
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
  // Everything else is irrelevant for these tests.
  Status Lookup(InodeID, std::string_view, SwordFsInode *) override {
    return Status::OK();
  }
  Status GetInode(InodeID, SwordFsInode *) override {
    return Status::OK();
  }
  Status ReadFileChunkSnapshot(InodeID ino, ChunkIndex index, swordfs::metadata::FileChunkSnapshot *out) override {
    if (!file_snapshot_status.ok()) {
      return file_snapshot_status;
    }
    if (out == nullptr) {
      return Status::InvalidArgument("typed chunk snapshot output is null");
    }
    swordfs::metadata::FileChunkSnapshot snapshot;
    snapshot.inode = SwordFsInode(ino, SwordFsAttr(ino, S_IFREG | 0644), ino);
    snapshot.inode.attr.size = file_size_;
    if (auto it = typed_mappings_.find(index); it != typed_mappings_.end()) {
      snapshot.chunk_id = it->second;
    }
    swordfs::metadata::ChunkSizeLayout layout;
    auto status = swordfs::metadata::PlanChunkSizeLayout(file_size_, chunk_size_, &layout);
    if (!status.ok()) {
      return status;
    }
    if (layout.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> boundary_id;
      if (auto it = typed_mappings_.find(layout.boundary->index); it != typed_mappings_.end()) {
        boundary_id = it->second;
      }
      snapshot.eof_boundary = swordfs::metadata::ChunkBoundarySnapshot{
          .index = layout.boundary->index, .chunk_id = boundary_id, .visible_prefix = layout.boundary->visible_prefix};
    }
    *out = std::move(snapshot);
    return Status::OK();
  }
  Status ReadFileMappingSnapshot(InodeID, swordfs::metadata::FileMappingSnapshot *) override {
    return Status::NotSupported("whole-file size mutations are outside these chunk-focused tests");
  }
  Status CommitShrink(InodeID, const swordfs::metadata::ChunkSizePlan &, const SwordFsAttr &, SetAttrField,
                      swordfs::metadata::ChunkSizeCommitResult *) override {
    return Status::NotSupported("whole-file size mutations are outside these chunk-focused tests");
  }
  Status CommitGrow(InodeID, const swordfs::metadata::ChunkSizePlan &, const SwordFsAttr &, SetAttrField,
                    swordfs::metadata::ChunkSizeCommitResult *) override {
    return Status::NotSupported("whole-file size mutations are outside these chunk-focused tests");
  }
  Status ProbeAttachment(InodeID, ChunkIndex index, std::optional<swordfs::metadata::ChunkID> *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("typed attachment output is null");
    }
    out->reset();
    if (auto it = typed_mappings_.find(index); it != typed_mappings_.end()) {
      *out = it->second;
    }
    return Status::OK();
  }
  Status AttachPrepared(InodeID, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                        const swordfs::metadata::FileSizePrecondition &expected) override {
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, chunk_size_);
    if (!status.ok()) {
      return status;
    }
    status = ValidateExpectedFileState(expected);
    if (!status.ok()) {
      return status;
    }
    if (!next_attach_status.ok()) {
      auto status = next_attach_status;
      next_attach_status = Status::OK();
      return status;
    }
    if (attach_mapping_without_eof_then_unknown) {
      attach_mapping_without_eof_then_unknown = false;
      typed_mappings_.emplace(index, id);
      return Status::OutcomeUnknown("mapping committed without the inode EOF update");
    }
    if (competing_attachment_.has_value()) {
      typed_mappings_[index] = competing_attachment_->first;
      file_size_ = std::max(file_size_, competing_attachment_->second);
      competing_attachment_.reset();
      if (competing_attach_then_outcome_unknown) {
        competing_attach_then_outcome_unknown = false;
        return Status::OutcomeUnknown("first attachment lost the race and Redis reply was lost");
      }
      return Status::AlreadyExists("another mount attached first");
    }
    if (typed_mappings_.contains(index)) {
      return Status::AlreadyExists("attachment already won");
    }
    typed_mappings_.emplace(index, id);
    file_size_ = std::max(file_size_, end);
    if (attach_then_outcome_unknown) {
      attach_then_outcome_unknown = false;
      return Status::OutcomeUnknown("first attachment committed but Redis response was lost");
    }
    return Status::OK();
  }
  // Simulate an independently committed FileMetadata replacement in tests.
  void ReplaceAttachment(ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t eof) {
    typed_mappings_[index] = id;
    file_size_ = eof;
  }
  void CompeteWithNextAttach(swordfs::metadata::ChunkID id, uint64_t eof) {
    competing_attachment_ = std::pair{id, eof};
  }
  Status FinalizeAttachedWrite(InodeID, ChunkIndex index, swordfs::metadata::ChunkID id, uint64_t end,
                               const swordfs::metadata::FileSizePrecondition &expected) override {
    if (always_finalize_conflict) {
      return Status::AlreadyExists("another mount keeps winning FileMetadata conflict");
    }
    if (!next_finalize_status.ok()) {
      auto status = next_finalize_status;
      next_finalize_status = Status::OK();
      return status;
    }
    auto status = swordfs::metadata::ValidateFileChunkWrite(index, id, end, chunk_size_);
    if (!status.ok()) {
      return status;
    }
    status = ValidateExpectedFileState(expected);
    if (!status.ok()) {
      return status;
    }
    auto it = typed_mappings_.find(index);
    if (it == typed_mappings_.end() || it->second != id) {
      return Status::AlreadyExists("attached identity changed");
    }
    file_size_ = std::max(file_size_, end);
    return Status::OK();
  }
  Status GetInodes(const std::vector<InodeID> &, std::vector<std::optional<SwordFsInode>> *) override {
    return Status::NotSupported("batch inode lookup is not used by this test double");
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
  Status SetAttr(InodeID, const SwordFsAttr &, SetAttrField, SwordFsInode *) override {
    return Status::OK();
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
      *size = 0;
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
  Status VisitPendingDeletesBatch(size_t, const swordfs::metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    if (has_more != nullptr) {
      *has_more = false;
    }
    return Status::OK();
  }
  Status CompletePendingDelete(std::string_view) override {
    return Status::OK();
  }
  Status OpenDir(InodeID, swordfs::metadata::DirIteratorPtr *) override {
    return Status::OK();
  }

 private:
  Status ValidateExpectedFileState(const swordfs::metadata::FileSizePrecondition &expected) const {
    auto status = swordfs::metadata::ValidateFileSizePrecondition(expected, chunk_size_);
    if (!status.ok()) {
      return status;
    }
    if (expected.eof != file_size_) {
      return Status::AlreadyExists("EOF changed");
    }
    if (expected.boundary.has_value()) {
      std::optional<swordfs::metadata::ChunkID> current;
      if (auto it = typed_mappings_.find(expected.boundary->index); it != typed_mappings_.end()) {
        current = it->second;
      }
      if (current != expected.boundary->chunk_id) {
        return Status::AlreadyExists("EOF boundary changed");
      }
    }
    return Status::OK();
  }

  uint64_t chunk_size_ = kChunkTestSize;
  uint64_t file_size_ = 0;
  std::map<ChunkIndex, swordfs::metadata::ChunkID> typed_mappings_;
  std::optional<std::pair<swordfs::metadata::ChunkID, uint64_t>> competing_attachment_;

 public:
  Status file_snapshot_status = Status::OK();
  Status next_attach_status = Status::OK();
  bool attach_mapping_without_eof_then_unknown = false;
  bool always_finalize_conflict = false;
  Status next_finalize_status = Status::OK();
  bool attach_then_outcome_unknown = false;
  bool competing_attach_then_outcome_unknown = false;
};

// In-memory object engine for bounded reads, write publication and fault injection.
class NullDataEngine final : public IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view key, std::unique_ptr<folly::IOBuf> data) override {
    ++put_calls;
    uploaded_keys.emplace_back(key);
    if (put_started_ != nullptr) {
      auto *started = put_started_;
      auto *release = put_release_;
      put_started_ = nullptr;
      put_release_ = nullptr;
      started->post();
      release->wait();
    }
    if (!next_put_status.ok()) {
      auto status = next_put_status;
      next_put_status = Status::OK();
      return status;
    }
    blobs_.insert_or_assign(std::string(key),
                            std::string(reinterpret_cast<const char *>(data->data()), data->length()));
    return Status::OK();
  }
  Status Get(std::string_view key, size_t offset, size_t size, folly::IOBuf *out) override {
    ++get_calls;
    requested_bytes += size;
    if (fail_get_call == get_calls) {
      return Status::IOError("injected later physical GET failure");
    }
    const auto it = blobs_.find(std::string(key));
    if (it == blobs_.end()) {
      return Status::NotFound("missing chunk object");
    }
    const auto &blob = it->second;
    if (offset < blob.size()) {
      const size_t available = std::min(size, blob.size() - offset);
      if (available > out->tailroom()) {
        return Status::InvalidArgument("insufficient object-read tailroom");
      }
      std::memcpy(out->writableTail(), blob.data() + offset, available);
      out->append(available);
    }
    return Status::OK();
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }

  void BlockNextPut(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    put_started_ = started;
    put_release_ = release;
  }

  int put_calls = 0;
  int get_calls = 0;
  int fail_get_call = -1;
  size_t requested_bytes = 0;
  std::vector<std::string> uploaded_keys;
  Status next_put_status = Status::OK();

 private:
  std::unordered_map<std::string, std::string> blobs_;
  folly::fibers::Baton *put_started_{nullptr};
  folly::fibers::Baton *put_release_{nullptr};
};

// Minimal mechanism-neutral test doubles for the active Slice runtime.
// Mount-level tests may independently inject VolumeImpl engines.
class ChunkRuntimeTest : public ::testing::Test {
 protected:
  void InitializeLocalDoubles() {
    local_meta_ = std::make_unique<MissingMetaEngine>();
    local_data_ = std::make_unique<NullDataEngine>();
    meta_ = local_meta_.get();
    data_ = local_data_.get();
  }

  MissingMetaEngine *meta_ = nullptr;
  NullDataEngine *data_ = nullptr;

 private:
  std::unique_ptr<MissingMetaEngine> local_meta_;
  std::unique_ptr<NullDataEngine> local_data_;
};

}  // namespace swordfs::test::chunk_test
