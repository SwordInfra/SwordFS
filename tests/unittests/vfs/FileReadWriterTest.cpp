// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FileReadWriter — read, write, flush, and
// cross-file-handle sharing behaviour.

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "chunk/Chunk.hpp"
#include "chunk/ChunkObjectKey.hpp"
#include "chunk/WholeObjectCleanup.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/IMetaEngine.hpp"
#include "metadata/Types.hpp"
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
using swordfs::utils::Status;
using swordfs::vfs::FileReadWriter;
using swordfs::vfs::InodeHandle;

// ────────────────────────────────────────────────────────────────
// Helpers
// ────────────────────────────────────────────────────────────────

static std::string Repeat(char c, size_t n) {
  return std::string(n, c);
}

static auto Buf(const std::string &s) {
  return *folly::IOBuf::copyBuffer(s.data(), s.size());
}

static void ConfigureWritebackConcurrencyForTest(int threads) {
  CLI::App app{"SwordFS FileReadWriter test"};
  auto &config = swordfs::config::ConfigCenter::Instance();
  config.Initialize();
  config.ConfigureOptions(app);

  std::vector<std::string> args = {
      "swordfs",
      "mount",
      "--volume",
      "test",
      "--meta",
      "memory://local",
      "--storage-thread-count",
      std::to_string(threads),
      "--meta-thread-count",
      std::to_string(threads),
      "/tmp/swordfs-test",
  };
  std::vector<const char *> argv;
  argv.reserve(args.size());
  for (const auto &arg : args) {
    argv.push_back(arg.c_str());
  }
  app.parse(static_cast<int>(argv.size()), argv.data());
}

template <typename Fn>
static void RunInTestFiber(Fn &&fn) {
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  fm.addTask([&] {
    fn();
    done.post();
  });
  while (!done.try_wait()) {
    evb.loopOnce();
  }
}

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
  Status GetInode(InodeID, SwordFsInode *out) override {
    if (!get_inode_status.ok()) {
      return get_inode_status;
    }
    const off_t snapshot_size = file_size_;
    if (get_inode_started_ != nullptr) {
      auto *started = get_inode_started_;
      auto *release = get_inode_release_;
      get_inode_started_ = nullptr;
      get_inode_release_ = nullptr;
      started->post();
      release->wait();
    }
    if (out) {
      *out = {};
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
  Status Unlink(InodeID, std::string_view) override {
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
  Status Open(InodeID, uint64_t *size = nullptr) override {
    if (size != nullptr) {
      *size = static_cast<uint64_t>(file_size_);
    }
    return Status::OK();
  }
  Status PrepareReclaim(InodeID, std::optional<swordfs::metadata::ReclaimWork> *work) override {
    work->reset();
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
        if (publish_chunk_status.IsAlreadyExists() || publish_chunk_status.IsNotFound()) {
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
      file_size_ = std::max(file_size_, static_cast<off_t>(replacement.start_offset + replacement.size));
      return publish_chunk_status;
    }

    ++replace_chunk_calls;
    if (!replace_chunk_status.ok() && !replace_chunk_commit_on_error && !replace_chunk_descriptor_only_on_error) {
      if (replace_chunk_status.IsAlreadyExists() || replace_chunk_status.IsNotFound()) {
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
      file_size_ = std::max(file_size_, static_cast<off_t>(replacement.start_offset + replacement.size));
      return replace_chunk_status;
    }
    if (!(it->second == *expected)) {
      QueuePendingDelete(ino, replacement);
      return Status::AlreadyExists("chunk changed before replacement");
    }
    QueuePendingDelete(ino, *expected);
    it->second = replacement;
    if (!replace_chunk_descriptor_only_on_error) {
      file_size_ = std::max(file_size_, static_cast<off_t>(replacement.start_offset + replacement.size));
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

  void BlockNextGetInode(folly::fibers::Baton *started, folly::fibers::Baton *release) {
    get_inode_started_ = started;
    get_inode_release_ = release;
  }

  std::vector<std::string> PendingDeleteKeys() const {
    std::vector<std::string> out;
    out.reserve(pending_deletes_.size());
    for (const auto &[id, pending] : pending_deletes_) {
      (void)id;
      swordfs::chunk::WholeObjectRef ref;
      auto status = swordfs::chunk::DecodeWholeObjectDelete(pending, 0, &ref);
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
    auto status = swordfs::chunk::FreezeWholeObjectDelete(ino, chunk, 0, &work);
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
      if (chunk.start_offset >= size) {
        QueuePendingDelete(ino, chunk);
        it = ino_it->second.erase(it);
        continue;
      }
      const uint64_t max_size = size - chunk.start_offset;
      if (chunk.size > max_size) {
        chunk.size = max_size;
      }
      ++it;
    }
  }

 private:
  off_t file_size_ = 0;
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
    ConfigureWritebackConcurrencyForTest(2);
    auto data = std::make_unique<MockDataEngine>();
    auto meta = std::make_unique<MockMetaEngine>();
    mock_data_ = data.get();
    mock_meta_ = meta.get();
    swordfs::volume::VolumeImpl::Initialize();
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_meta_engine(std::move(meta));
    vol.set_data_engine(std::move(data));
  }

  FileReadWriter Make(off_t file_size = 0) {
    mock_meta_->set_file_size(file_size);
    return FileReadWriter(kIno);
  }

  static constexpr size_t kChunkSize = 1024;
  static constexpr InodeID kIno = 42;
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
    SwordFsChunk chunk{};
    chunk.index = 0;
    chunk.start_offset = 0;
    chunk.revision = 7;
    chunk.size = 300;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());

    auto persisted = std::make_unique<folly::IOBuf>(Buf(Repeat('P', chunk.size)));
    ASSERT_TRUE(
        mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(persisted))
            .ok());

    auto rw = Make(chunk.size);
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(rw.Read(kChunkSize, 0, out.get()).ok());
    EXPECT_EQ(out->length(), chunk.size);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('P', chunk.size));
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

    EXPECT_EQ(status.code(), Status::kInvalidArgument);
    EXPECT_EQ(status.message(), "FileReadWriter::Read: negative offset");
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, VisibleSizeMetadataFailurePropagatesFromRead) {
  RunInTestFiber([&] {
    auto rw = Make(64);
    mock_meta_->get_inode_status = Status::IOError("injected inode-size lookup failure");
    auto out = folly::IOBuf::create(64);

    auto status = rw.Read(64, 0, out.get());

    EXPECT_EQ(status.code(), Status::kIOError);
    EXPECT_EQ(status.message(), "injected inode-size lookup failure");
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

  while (!flush_done.try_wait()) {
    evb.loopOnce();
  }
  ASSERT_TRUE(write_status.ok()) << write_status.message();
  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  ASSERT_EQ(mock_meta_->file_size(), 13);

  release_get_inode.post();
  while (!read_done.try_wait()) {
    evb.loopOnce();
  }
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
    mock_meta_->find_chunk_status = Status::NotFound("chunk is not materialized");

    auto out = folly::IOBuf::create(64);
    auto status = rw.Read(64, 0, out.get());

    ASSERT_TRUE(status.ok()) << status.message();
    ASSERT_EQ(out->length(), 64);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), std::string(64, '\0'));
  });
}

TEST_F(FileReadWriterTest, PersistedChunkShortReadFailsClosed) {
  RunInTestFiber([&] {
    SwordFsChunk chunk{};
    chunk.index = 0;
    chunk.start_offset = 0;
    chunk.revision = 17;
    chunk.size = 64;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());

    auto persisted = std::make_unique<folly::IOBuf>(Buf(Repeat('S', 32)));
    ASSERT_TRUE(
        mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(persisted))
            .ok());

    auto rw = Make(chunk.size);
    auto out = folly::IOBuf::create(chunk.size);
    auto status = rw.Read(chunk.size, 0, out.get());

    EXPECT_EQ(status.code(), Status::kIOError);
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, PersistedChunkReadErrorRollsBackPartialOutput) {
  RunInTestFiber([&] {
    SwordFsChunk published{};
    published.index = 0;
    published.start_offset = 0;
    published.revision = 18;
    published.size = 64;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, published).ok());

    mock_data_->get_error_payload = "partial";
    mock_data_->get_status = Status::IOError("injected data read failure");

    swordfs::chunk::Chunk chunk(kIno, 0);
    ASSERT_TRUE(chunk.Initialize().ok());

    auto out = folly::IOBuf::create(68);
    std::memcpy(out->writableTail(), "keep", 4);
    out->append(4);
    auto status = chunk.Read(0, published.size, out.get());

    EXPECT_EQ(status.code(), Status::kIOError);
    EXPECT_EQ(status.message(), "injected data read failure");
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "keep");
  });
}

TEST_F(FileReadWriterTest, PersistedChunkZeroLengthReadIsNoOp) {
  RunInTestFiber([&] {
    SwordFsChunk published{};
    published.index = 0;
    published.start_offset = 0;
    published.revision = 19;
    published.size = 64;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, published).ok());

    swordfs::chunk::Chunk chunk(kIno, 0);
    ASSERT_TRUE(chunk.Initialize().ok());

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

    EXPECT_EQ(status.code(), Status::kIOError);
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

    EXPECT_TRUE(status.IsMalformed());
    EXPECT_EQ(status.message(), "injected malformed chunk metadata");
    EXPECT_EQ(out->length(), 0);
  });
}

TEST_F(FileReadWriterTest, ChunkMetadataIOErrorPropagatesFromWrite) {
  RunInTestFiber([&] {
    auto rw = Make();
    mock_meta_->find_chunk_status = Status::IOError("injected metadata write lookup failure");

    auto status = rw.Write(Buf("data"), 0);

    EXPECT_EQ(status.code(), Status::kIOError);
    EXPECT_EQ(status.message(), "injected metadata write lookup failure");
  });
}

TEST_F(FileReadWriterTest, CrossChunkMetadataErrorDrainsSubmittedReadBeforeReturning) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

  SwordFsChunk first{};
  first.index = 0;
  first.start_offset = 0;
  first.revision = 17;
  first.size = kChunkSize;
  ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, first).ok());
  auto data = std::make_unique<folly::IOBuf>(Buf(Repeat('R', kChunkSize)));
  ASSERT_TRUE(
      mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, first.index, first.revision), std::move(data)).ok());

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

  while (!get_started.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_FALSE(read_done.try_wait());

  release_get.post();
  while (!read_done.try_wait()) {
    evb.loopOnce();
  }

  EXPECT_EQ(read_status.code(), Status::kIOError);
  EXPECT_EQ(read_status.message(), "injected second-chunk metadata failure");
  EXPECT_EQ(out->length(), 0);
  vol.clear_chunk_size_for_test();
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
    SwordFsChunk unpublished;
    EXPECT_TRUE(mock_meta_->FindChunk(kIno, 0, &unpublished).IsNotFound());
    EXPECT_TRUE(mock_data_->StoredKeys().empty());
    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);
    EXPECT_EQ(mock_meta_->file_size(), 6);

    SwordFsChunk chunk;
    EXPECT_TRUE(mock_meta_->FindChunk(kIno, 0, &chunk).ok());
    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reopened.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
  });
}

TEST_F(FileReadWriterTest, PutFailureRetryReconcilesMetadataBeforeRepublishing) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_EQ(mock_meta_->find_chunk_calls, 1);

    mock_data_->put_status = Status::IOError("injected put failure");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_EQ(mock_meta_->find_chunk_calls, 1);

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->find_chunk_calls, 2);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);
    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_EQ(published.revision, 2U);
  });
}

TEST_F(FileReadWriterTest, SuccessfulFirstFlushDoesNotRefreshMetadataBaseline) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_EQ(mock_meta_->find_chunk_calls, 1);

    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->find_chunk_calls, 1);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 1);
    EXPECT_EQ(mock_data_->put_calls, 1);
  });
}

TEST_F(FileReadWriterTest, FlushRetriesRevisionAllocationFailureBeforeUploadingObject) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_EQ(mock_meta_->find_chunk_calls, 1);

    mock_meta_->allocate_chunk_revision_status = Status::IOError("revision allocation failed");
    const auto failed = rw.Flush();
    EXPECT_EQ(failed.code(), Status::kIOError);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 1);
    EXPECT_EQ(mock_data_->put_calls, 0);
    EXPECT_EQ(mock_meta_->find_chunk_calls, 1);
    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    mock_meta_->allocate_chunk_revision_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);
    EXPECT_EQ(mock_data_->put_calls, 1);
    EXPECT_EQ(mock_meta_->find_chunk_calls, 2);

    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_NE(published.revision, swordfs::metadata::kInvalidChunkRevision);
    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reopened.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
  });
}

TEST_F(FileReadWriterTest, CompetingInitialPublishersUseDistinctRevisionsAndObjects) {
  RunInTestFiber([&] {
    FileReadWriter first(kIno);
    FileReadWriter second(kIno);
    ASSERT_TRUE(first.Write(Buf("first"), 0).ok());
    ASSERT_TRUE(second.Write(Buf("second"), 0).ok());

    ASSERT_TRUE(first.Flush().ok());
    SwordFsChunk winner;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &winner).ok());
    ASSERT_EQ(winner.revision, 1U);

    const auto status = second.Flush();
    EXPECT_TRUE(status.IsAlreadyExists());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);

    const auto winner_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, winner.revision);
    const auto loser_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, 2);
    EXPECT_NE(winner_key, loser_key);
    const auto stored_keys = mock_data_->StoredKeys();
    EXPECT_NE(std::find(stored_keys.begin(), stored_keys.end(), winner_key), stored_keys.end());
    EXPECT_NE(std::find(stored_keys.begin(), stored_keys.end(), loser_key), stored_keys.end());
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(), std::vector<std::string>{loser_key});
  });
}

TEST_F(FileReadWriterTest, FlushRetriesPublicationFailureWithFreshRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    const std::string payload = Repeat('M', 128);
    ASSERT_TRUE(rw.Write(Buf(payload), 0).ok());

    mock_meta_->publish_chunk_status = Status::IOError("injected publication failure");
    EXPECT_FALSE(rw.Flush().ok());
    SwordFsChunk unpublished;
    EXPECT_TRUE(mock_meta_->FindChunk(kIno, 0, &unpublished).IsNotFound());
    EXPECT_EQ(mock_data_->StoredKeys().size(), 1U);
    EXPECT_FALSE(rw.Flush().ok());
    EXPECT_TRUE(mock_meta_->FindChunk(kIno, 0, &unpublished).IsNotFound());
    EXPECT_EQ(mock_data_->StoredKeys().size(), 2U);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);

    mock_meta_->publish_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);
    EXPECT_EQ(mock_data_->StoredKeys().size(), 3U);
    EXPECT_EQ(mock_meta_->file_size(), static_cast<off_t>(payload.size()));
    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_EQ(published.revision, 3U);

    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(payload.size());
    ASSERT_TRUE(reopened.Read(payload.size(), 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), payload);
  });
}

TEST_F(FileReadWriterTest, FlushRetryAfterAmbiguousInitialCommitPublishesFreshRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    const std::string payload = Repeat('A', 128);
    ASSERT_TRUE(rw.Write(Buf(payload), 0).ok());

    mock_meta_->publish_chunk_commit_on_error = true;
    mock_meta_->publish_chunk_status = Status::IOError("response lost after commit");
    EXPECT_FALSE(rw.Flush().ok());
    ASSERT_EQ(mock_data_->put_calls, 1);
    ASSERT_EQ(mock_meta_->file_size(), static_cast<off_t>(payload.size()));

    mock_meta_->publish_chunk_commit_on_error = false;
    mock_meta_->publish_chunk_status = Status::OK();
    EXPECT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 2);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);
    EXPECT_EQ(mock_meta_->replace_chunk_calls, 1);
    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_EQ(published.revision, 2U);
  });
}

TEST_F(FileReadWriterTest, FlushRebasesChangedAuthoritativeChunkAndPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    std::string payload = Repeat('C', 128);
    ASSERT_TRUE(rw.Write(Buf(payload), 0).ok());

    mock_meta_->publish_chunk_status = Status::IOError("injected publication failure");
    EXPECT_FALSE(rw.Flush().ok());
    ASSERT_EQ(mock_data_->put_calls, 1);
    ASSERT_TRUE(rw.Write(Buf("L"), 0).ok());
    payload[0] = 'L';

    SwordFsChunk conflicting{};
    conflicting.index = 0;
    conflicting.start_offset = 0;
    conflicting.revision = 999;
    conflicting.size = payload.size();
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, conflicting).ok());

    mock_meta_->publish_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 2);
    EXPECT_EQ(mock_data_->StoredKeys().size(), 2U);
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(),
              std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(kIno, 0, conflicting.revision)});
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 1000U);
    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(payload.size());
    ASSERT_TRUE(reopened.Read(payload.size(), 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), payload);
  });
}

TEST_F(FileReadWriterTest, InitialPublishRetryNeverReusesRejectedRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());

    mock_meta_->publish_chunk_status = Status::AlreadyExists("winner already published");
    ASSERT_TRUE(rw.Flush().IsAlreadyExists());

    const auto losing_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, 1);
    ASSERT_EQ(mock_meta_->PendingDeleteKeys(), std::vector<std::string>{losing_key});
    ASSERT_EQ(mock_meta_->allocate_chunk_revision_calls, 1);
    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());

    // A definite rejection ends only this publication attempt, not local
    // writability. The rejected revision is terminal because best-effort
    // cleanup may race any reuse of the same immutable object key.
    mock_meta_->publish_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 2U);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(), std::vector<std::string>{losing_key});

    const auto current_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, current.revision);
    const auto stored_keys = mock_data_->StoredKeys();
    EXPECT_NE(std::find(stored_keys.begin(), stored_keys.end(), current_key), stored_keys.end());

    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(5);
    ASSERT_TRUE(reopened.Read(5, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello");
  });
}

TEST_F(FileReadWriterTest, RewriteDefiniteRejectionAllocatesFreshRevisionOnRetry) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->replace_chunk_status = Status::AlreadyExists("injected definite rejection");
    ASSERT_TRUE(rw.Flush().IsAlreadyExists());
    const auto rejected_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, 2);
    ASSERT_EQ(mock_meta_->PendingDeleteKeys(), std::vector<std::string>{rejected_key});

    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 3U);
    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, FlushNeverShrinksExistingFileSize) {
  RunInTestFiber([&] {
    auto rw = Make(4096);
    ASSERT_TRUE(rw.Write(Buf(Repeat('S', 128)), 0).ok());
    ASSERT_EQ(mock_meta_->find_chunk_calls, 1);
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->find_chunk_calls, 1);
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

  while (!flush_done.try_wait()) {
    evb.loopOnce();
  }
  ASSERT_TRUE(flush_status.ok()) << flush_status.message();
  ASSERT_EQ(mock_meta_->file_size(), 13);

  release_get_inode.post();
  while (!get_attr_done.try_wait()) {
    evb.loopOnce();
  }
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

TEST_F(FileReadWriterTest, FailedFlushKeepsTransientLiveSize) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("Hello,_World!"), 0).ok());
    mock_data_->put_status = Status::IOError("injected publication failure");

    auto status = rw.Flush();
    ASSERT_EQ(status.code(), Status::kIOError);
    ASSERT_EQ(mock_meta_->file_size(), 0);

    SwordFsInode inode;
    ASSERT_TRUE(rw.GetAttr(&inode).ok());
    EXPECT_EQ(inode.attr.size, 13U);
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
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_chunk_size_for_test(kChunkSize);
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf(Repeat('A', kChunkSize)), 0).ok());
    ASSERT_TRUE(rw.Write(Buf(Repeat('B', 128)), static_cast<off_t>(kChunkSize)).ok());

    mock_data_->put_status = Status::IOError("injected first-chunk failure");
    mock_data_->fail_put_prefix = std::to_string(kIno) + "/0/";
    EXPECT_FALSE(rw.Flush().ok());

    SwordFsChunk chunk;
    EXPECT_TRUE(mock_meta_->FindChunk(kIno, 1, &chunk).ok());
    EXPECT_TRUE(mock_meta_->FindChunk(kIno, 0, &chunk).IsNotFound());

    mock_data_->put_status = Status::OK();
    mock_data_->fail_put_prefix.clear();
    EXPECT_TRUE(rw.Flush().ok());
    EXPECT_TRUE(mock_meta_->FindChunk(kIno, 0, &chunk).ok());
    EXPECT_EQ(mock_meta_->file_size(), static_cast<off_t>(kChunkSize + 128));
    vol.clear_chunk_size_for_test();
  });
}

TEST_F(FileReadWriterTest, SetAttrTruncateOrdersAfterAmbiguousFlushRetry) {
  RunInTestFiber([&] {
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_chunk_size_for_test(kChunkSize);

    std::shared_ptr<swordfs::vfs::FileHandle> handle;
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &handle).ok());
    const std::string payload = Repeat('T', 200);
    ASSERT_TRUE(handle->Write(Buf(payload), 0).ok());

    mock_meta_->publish_chunk_commit_on_error = true;
    mock_meta_->publish_chunk_status = Status::IOError("response lost after commit");
    EXPECT_FALSE(handle->Flush().ok());
    ASSERT_EQ(mock_data_->put_calls, 1);

    struct stat attr{};
    attr.st_size = 64;
    ASSERT_TRUE(
        swordfs::vfs::VfsImpl::SetAttr(kIno, &attr, static_cast<int>(SetAttrField::kSize), std::nullopt, nullptr).ok());

    mock_meta_->publish_chunk_commit_on_error = false;
    mock_meta_->publish_chunk_status = Status::OK();
    ASSERT_TRUE(handle->Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 2);
    EXPECT_EQ(mock_meta_->file_size(), 64);

    SwordFsChunk chunk;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &chunk).ok());
    EXPECT_EQ(chunk.revision, 2U);
    EXPECT_EQ(chunk.size, 64);

    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(128);
    ASSERT_TRUE(reopened.Read(128, 0, out.get()).ok());
    const std::string expected = Repeat('T', 64);
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), expected);

    ASSERT_TRUE(handle->Release().ok());
    vol.clear_chunk_size_for_test();
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
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened2).ok());
    fh1 = opened1->fh();
    fh2 = opened2->fh();

    auto h1 = mgr.FindAs<swordfs::vfs::FileHandle>(fh1);
    auto h2 = mgr.FindAs<swordfs::vfs::FileHandle>(fh2);
    ASSERT_NE(h1, nullptr);
    ASSERT_NE(h2, nullptr);
    EXPECT_EQ(h1->handle().get(), h2->handle().get());

    // Write through handle 1, read through handle 2 (same instance).
    ASSERT_TRUE(h1->Write(Buf(Repeat('Z', 300)), 100).ok());
    auto out = folly::IOBuf::create(kChunkSize);
    ASSERT_TRUE(h2->Read(300, 100, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('Z', 300));

    ASSERT_TRUE(h1->Release().ok());
    ASSERT_TRUE(h2->Release().ok());
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
    ASSERT_TRUE(swordfs::vfs::FileHandle::Open(kIno, 0, &opened2).ok());
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

    ASSERT_TRUE(h1->Release().ok());
    ASSERT_TRUE(h2->Release().ok());
  });
}

TEST_F(FileReadWriterTest, WriteAfterFlushOverwritesExistingChunkWithoutLosingUntouchedBytes) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reopened(kIno);
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

    FileReadWriter reopened(kIno);
    ASSERT_TRUE(reopened.Write(Buf(" world"), 5).ok());
    ASSERT_TRUE(reopened.Flush().ok());

    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "hello world");
    EXPECT_EQ(mock_meta_->file_size(), 11);
  });
}

TEST_F(FileReadWriterTest, RewritePublishesVersionedObjectAndQueuesOldRevisionForCleanup) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &first).ok());
    ASSERT_EQ(first.revision, 1U);

    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk replacement;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &replacement).ok());
    EXPECT_GT(replacement.revision, first.revision);
    EXPECT_EQ(first.revision, 1U);
    EXPECT_EQ(replacement.revision, 2U);
    EXPECT_EQ(replacement.size, 11U);
    const auto first_key = swordfs::chunk::FormatChunkObjectKey(kIno, first.index, first.revision);
    const auto replacement_key = swordfs::chunk::FormatChunkObjectKey(kIno, replacement.index, replacement.revision);
    EXPECT_EQ(mock_data_->StoredKeys(), (std::vector<std::string>{first_key, replacement_key}));
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(), std::vector<std::string>{first_key});
  });
}

TEST_F(FileReadWriterTest, RewritePutFailureKeepsOldVersionAuthoritativeAndCanRetry) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &first).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_data_->put_status = Status::IOError("rewrite put failed");
    EXPECT_TRUE(rw.Flush().code() == Status::kIOError);

    SwordFsChunk still_first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &still_first).ok());
    EXPECT_EQ(still_first, first);
    FileReadWriter old_reader(kIno);
    auto old_out = folly::IOBuf::create(11);
    ASSERT_TRUE(old_reader.Read(11, 0, old_out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(old_out->data()), old_out->length()), "hello world");

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);
    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_EQ(published.revision, 3U);
    FileReadWriter new_reader(kIno);
    auto new_out = folly::IOBuf::create(11);
    ASSERT_TRUE(new_reader.Read(11, 0, new_out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(new_out->data()), new_out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, RewriteRetryAfterAmbiguousCommitPublishesFreshRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_EQ(mock_data_->put_calls, 1);

    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    mock_meta_->replace_chunk_commit_on_error = true;
    mock_meta_->replace_chunk_status = Status::IOError("replacement reply lost after commit");
    EXPECT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_EQ(mock_data_->put_calls, 2);
    ASSERT_EQ(mock_meta_->replace_chunk_calls, 1);

    mock_meta_->replace_chunk_commit_on_error = false;
    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 3);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);
    EXPECT_EQ(mock_meta_->replace_chunk_calls, 2);
    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_EQ(published.revision, 3U);

    FileReadWriter reader(kIno);
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

    mock_meta_->replace_chunk_commit_on_error = true;
    mock_meta_->replace_chunk_status = Status::IOError("replacement reply lost after commit");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);

    mock_meta_->replace_chunk_commit_on_error = false;
    mock_meta_->replace_chunk_status = Status::IOError("fresh retry failed");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 4);
    EXPECT_EQ(mock_data_->put_calls, 4);

    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_EQ(published.revision, 4U);

    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(12);
    ASSERT_TRUE(reader.Read(12, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world!");
  });
}

TEST_F(FileReadWriterTest, RewriteRetryRepairsMetadataSideEffectsAfterPartialCommit) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_EQ(mock_meta_->file_size(), 5);
    ASSERT_EQ(mock_data_->put_calls, 1);

    ASSERT_TRUE(rw.Write(Buf(" world"), 5).ok());
    mock_meta_->replace_chunk_descriptor_only_on_error = true;
    mock_meta_->replace_chunk_status = Status::IOError("inode side effect failed after descriptor commit");
    EXPECT_EQ(rw.Flush().code(), Status::kIOError);
    EXPECT_EQ(mock_meta_->file_size(), 5);
    ASSERT_EQ(mock_data_->put_calls, 2);
    ASSERT_EQ(mock_meta_->replace_chunk_calls, 1);

    SwordFsChunk replacement;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &replacement).ok());
    EXPECT_EQ(replacement.size, 11U);

    mock_meta_->replace_chunk_descriptor_only_on_error = false;
    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 3);
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);
    EXPECT_EQ(mock_meta_->replace_chunk_calls, 2);
    EXPECT_EQ(mock_meta_->file_size(), 11);

    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "hello world");
  });
}

TEST_F(FileReadWriterTest, RewriteRebasesChangedDescriptorWithoutForegroundDelete) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    ASSERT_TRUE(rw.Write(Buf(" world"), 5).ok());
    mock_meta_->replace_chunk_descriptor_only_on_error = true;
    mock_meta_->replace_chunk_status = Status::IOError("descriptor committed, inode side effect failed");
    EXPECT_EQ(rw.Flush().code(), Status::kIOError);

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    ASSERT_EQ(current.size, 11U);
    ASSERT_GT(current.revision, 1U);

    current.size = 10;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, current).ok());
    mock_meta_->replace_chunk_descriptor_only_on_error = false;
    mock_meta_->replace_chunk_status = Status::OK();

    ASSERT_TRUE(rw.Flush().ok());
    const auto stored_keys = mock_data_->StoredKeys();
    const auto current_key = swordfs::chunk::FormatChunkObjectKey(kIno, current.index, current.revision);
    EXPECT_NE(std::find(stored_keys.begin(), stored_keys.end(), current_key), stored_keys.end());
    EXPECT_EQ(std::find(mock_data_->delete_calls.begin(), mock_data_->delete_calls.end(), current_key),
              mock_data_->delete_calls.end());
    const auto pending_deletes = mock_meta_->PendingDeleteKeys();
    EXPECT_NE(std::find(pending_deletes.begin(), pending_deletes.end(), current_key), pending_deletes.end());

    SwordFsChunk replacement;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &replacement).ok());
    EXPECT_EQ(replacement.revision, 3U);
    EXPECT_EQ(replacement.size, 11U);
  });
}

TEST_F(FileReadWriterTest, RewriteHydrationPropagatesBackendReadFailure) {
  RunInTestFiber([&] {
    SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 77, .size = 11};
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());
    auto data = std::make_unique<folly::IOBuf>(Buf("hello world"));
    ASSERT_TRUE(
        mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(data)).ok());

    mock_data_->get_status = Status::IOError("hydrate read failed");
    auto rw = Make(11);
    const auto status = rw.Write(Buf("H"), 0);
    EXPECT_EQ(status.code(), Status::kIOError);
    EXPECT_EQ(status.message(), "hydrate read failed");
  });
}

TEST_F(FileReadWriterTest, RewriteHydrationRejectsObjectShorterThanDescriptor) {
  RunInTestFiber([&] {
    SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 77, .size = 11};
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());
    auto data = std::make_unique<folly::IOBuf>(Buf("short"));
    ASSERT_TRUE(
        mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(data)).ok());

    auto rw = Make(11);
    const auto status = rw.Write(Buf("H"), 0);
    EXPECT_EQ(status.code(), Status::kIOError);
    EXPECT_NE(status.message().find("shorter than metadata descriptor"), std::string::npos);
  });
}

TEST_F(FileReadWriterTest, RewriteHydrationRejectsDescriptorLargerThanChunk) {
  RunInTestFiber([&] {
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_chunk_size_for_test(8);

    SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 77, .size = 9};
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());
    auto rw = Make(9);
    const auto status = rw.Write(Buf("H"), 0);
    EXPECT_TRUE(status.IsMalformed());

    vol.clear_chunk_size_for_test();
  });
}

TEST_F(FileReadWriterTest, RewriteHydratesZeroLengthPublishedChunkWithoutBackendRead) {
  RunInTestFiber([&] {
    SwordFsChunk chunk{.index = 0, .start_offset = 0, .revision = 77, .size = 0};
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());

    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("new"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(3);
    ASSERT_TRUE(reader.Read(3, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "new");
  });
}

TEST_F(FileReadWriterTest, WriteAfterFailedInitialCommitPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    mock_meta_->publish_chunk_status = Status::IOError("publication failed");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);

    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    mock_meta_->publish_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);

    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(6);
    ASSERT_TRUE(reopened.Read(6, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "Hello!");
  });
}

TEST_F(FileReadWriterTest, WriteAfterAmbiguousInitialCommitRebasesAndPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());

    mock_meta_->publish_chunk_commit_on_error = true;
    mock_meta_->publish_chunk_status = Status::IOError("response lost after commit");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    SwordFsChunk committed;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &committed).ok());
    ASSERT_EQ(committed.revision, 1U);

    ASSERT_TRUE(rw.Write(Buf("H"), 0).ok());
    ASSERT_TRUE(rw.Write(Buf("!"), 5).ok());

    mock_meta_->publish_chunk_commit_on_error = false;
    mock_meta_->publish_chunk_status = Status::OK();
    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 2U);
    FileReadWriter reopened(kIno);
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

    mock_meta_->replace_chunk_status = Status::IOError("replacement failed");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);

    FileReadWriter reopened(kIno);
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

    mock_meta_->replace_chunk_commit_on_error = true;
    mock_meta_->replace_chunk_status = Status::IOError("replacement reply lost after commit");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);

    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    mock_meta_->replace_chunk_commit_on_error = false;
    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);

    FileReadWriter reopened(kIno);
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

    mock_meta_->replace_chunk_status = Status::IOError("replacement failed");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);

    mock_meta_->replace_chunk_status = Status::OK();
    mock_meta_->find_chunk_status = Status::IOError("retry lookup failed");
    const auto status = rw.Flush();
    EXPECT_EQ(status.code(), Status::kIOError);
    EXPECT_EQ(status.message(), "retry lookup failed");

    mock_meta_->find_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);

    SwordFsChunk published;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &published).ok());
    EXPECT_EQ(published.revision, 3U);
  });
}

TEST_F(FileReadWriterTest, RewriteRetryRebasesChangedDescriptorAndPublishesLatestLocalData) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &first).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());
    mock_meta_->replace_chunk_status = Status::IOError("replacement failed");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_TRUE(rw.Write(Buf("!"), 11).ok());

    const auto keys = mock_data_->StoredKeys();
    ASSERT_EQ(keys.size(), 2U);
    const auto first_key = swordfs::chunk::FormatChunkObjectKey(kIno, first.index, first.revision);
    const auto pending = keys[0] == first_key ? keys[1] : keys[0];

    auto winner = first;
    winner.revision = 999;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, winner).ok());
    mock_meta_->replace_chunk_status = Status::OK();

    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(std::find(mock_data_->delete_calls.begin(), mock_data_->delete_calls.end(), pending),
              mock_data_->delete_calls.end());
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(),
              std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(kIno, 0, winner.revision)});
    EXPECT_TRUE(mock_meta_->complete_pending_delete_calls.empty());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 1000U);
    EXPECT_EQ(current.size, 12U);
    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(12);
    ASSERT_TRUE(reopened.Read(12, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world!");
  });
}

TEST_F(FileReadWriterTest, RewriteRetryMissingExpectedPublishesFreshInitialRevision) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &first).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->replace_chunk_status = Status::IOError("publication outcome unavailable");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_EQ(mock_meta_->allocate_chunk_revision_calls, 2);

    const auto rejected_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, 2);
    mock_meta_->EraseChunkForTest(kIno, 0);
    mock_meta_->replace_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_meta_->allocate_chunk_revision_calls, 3);
    EXPECT_TRUE(mock_meta_->PendingDeleteKeys().empty());
    EXPECT_EQ(std::find(mock_data_->delete_calls.begin(), mock_data_->delete_calls.end(), rejected_key),
              mock_data_->delete_calls.end());

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 3U);
    FileReadWriter reopened(kIno);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reopened.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, RewriteRetryAfterFailedPutRebasesWithoutDeletingFailedCandidate) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &first).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_data_->put_status = Status::IOError("rewrite put failed");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_EQ(mock_data_->put_calls, 2);

    auto winner = first;
    winner.revision = 999;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, winner).ok());
    mock_data_->put_status = Status::OK();

    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 3);
    const auto unuploaded_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, 2);
    EXPECT_EQ(std::find(mock_data_->delete_calls.begin(), mock_data_->delete_calls.end(), unuploaded_key),
              mock_data_->delete_calls.end());
    const auto stored_keys = mock_data_->StoredKeys();
    EXPECT_EQ(std::find(stored_keys.begin(), stored_keys.end(), unuploaded_key), stored_keys.end());
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(),
              std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(kIno, 0, winner.revision)});

    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 1000U);
  });
}

TEST_F(FileReadWriterTest, RewriteDefiniteNotFoundQueuesLosingObjectWithoutForegroundDelete) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->replace_chunk_status = Status::NotFound("expected removed before publication");
    const auto status = rw.Flush();
    EXPECT_TRUE(status.IsNotFound());

    const auto losing_key = swordfs::chunk::FormatChunkObjectKey(kIno, 0, 2);
    EXPECT_EQ(std::find(mock_data_->delete_calls.begin(), mock_data_->delete_calls.end(), losing_key),
              mock_data_->delete_calls.end());
    EXPECT_TRUE(mock_meta_->complete_pending_delete_calls.empty());
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(), std::vector<std::string>{losing_key});
  });
}

TEST_F(FileReadWriterTest, RewriteRetryRemainsRetryableAfterDifferentDescriptorObservationRace) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &first).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->replace_chunk_status = Status::IOError("publication result ambiguous");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_EQ(mock_data_->put_calls, 2);

    auto replacement = first;
    replacement.revision = 2;
    replacement.size = 11;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, replacement).ok());

    auto observed_winner = first;
    observed_winner.revision = 999;
    mock_meta_->SetNextFindChunkResult(observed_winner);
    mock_meta_->replace_chunk_status = Status::OK();

    ASSERT_TRUE(rw.Flush().IsAlreadyExists());
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 4);
    // A failed retry attempt must not wedge the chunk; after reconciliation
    // converges, it becomes clean and can be made dirty by a new write again.
    EXPECT_TRUE(rw.Write(Buf("h"), 0).ok());
    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 1001U);
  });
}

TEST_F(FileReadWriterTest, RewriteRetryRemainsRetryableAfterMissingObservationRace) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());

    SwordFsChunk first;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &first).ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->replace_chunk_status = Status::IOError("publication result ambiguous");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_EQ(mock_data_->put_calls, 2);

    auto replacement = first;
    replacement.revision = 2;
    replacement.size = 11;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, replacement).ok());
    mock_meta_->SetNextFindChunkStatus(Status::NotFound("transiently missing"));
    mock_meta_->replace_chunk_status = Status::OK();

    ASSERT_TRUE(rw.Flush().IsAlreadyExists());
    ASSERT_TRUE(rw.Flush().ok());
    EXPECT_EQ(mock_data_->put_calls, 4);
    // The transient observation may fail one attempt, but the retained local
    // buffer remains retryable and writable after convergence.
    EXPECT_TRUE(rw.Write(Buf("h"), 0).ok());
    SwordFsChunk current;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &current).ok());
    EXPECT_EQ(current.revision, 4U);
  });
}

TEST_F(FileReadWriterTest, PartialTruncateErrorRebasesLatestLocalDataOnRetry) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    mock_meta_->truncate_commit_on_error = true;
    mock_meta_->set_truncate_status(Status::IOError("truncate reply lost after metadata mutation"));
    ASSERT_EQ(rw.Truncate(5).code(), Status::kIOError);

    SwordFsChunk truncated;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &truncated).ok());
    ASSERT_EQ(truncated.size, 5U);

    mock_meta_->truncate_commit_on_error = false;
    mock_meta_->set_truncate_status(Status::OK());
    ASSERT_TRUE(rw.Flush().IsAlreadyExists());
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
  });
}

TEST_F(FileReadWriterTest, PartialSetAttrErrorRebasesLatestLocalDataOnRetry) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello world"), 0).ok());
    ASSERT_TRUE(rw.Flush().ok());
    ASSERT_TRUE(rw.Write(Buf("HELLO"), 0).ok());

    SwordFsAttr attr{};
    attr.size = 5;
    mock_meta_->set_attr_commit_on_error = true;
    mock_meta_->set_attr_status = Status::IOError("setattr reply lost after metadata mutation");
    ASSERT_EQ(rw.SetAttr(attr, SetAttrField::kSize, nullptr).code(), Status::kIOError);

    SwordFsChunk truncated;
    ASSERT_TRUE(mock_meta_->FindChunk(kIno, 0, &truncated).ok());
    ASSERT_EQ(truncated.size, 5U);

    mock_meta_->set_attr_commit_on_error = false;
    mock_meta_->set_attr_status = Status::OK();
    ASSERT_TRUE(rw.Flush().IsAlreadyExists());
    ASSERT_TRUE(rw.Flush().ok());

    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(11);
    ASSERT_TRUE(reader.Read(11, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO world");
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

    FileReadWriter reader(kIno);
    auto out = folly::IOBuf::create(5);
    ASSERT_TRUE(reader.Read(5, 0, out.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), "HELLO");
  });
}

TEST_F(FileReadWriterTest, TruncateAfterAmbiguousInitialPublicationMayLeaveUploadedObject) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Write(Buf("hello"), 0).ok());
    mock_meta_->publish_chunk_status = Status::IOError("publication failed");
    ASSERT_EQ(rw.Flush().code(), Status::kIOError);
    ASSERT_EQ(mock_data_->StoredKeys(), std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(kIno, 0, 1)});

    mock_meta_->publish_chunk_status = Status::OK();
    ASSERT_TRUE(rw.Truncate(0).ok());
    // The earlier publication result was ambiguous and no authoritative chunk
    // exists to classify this object. Exceptional orphan completeness is not
    // part of truncate correctness, so the object may remain as garbage.
    EXPECT_EQ(mock_data_->StoredKeys(), std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(kIno, 0, 1)});
    EXPECT_TRUE(mock_data_->delete_calls.empty());
  });
}

// ────────────────────────────────────────────────────────────────
// Truncate
// ────────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, TruncateCallsMetaEngine) {
  RunInTestFiber([&] {
    auto rw = Make();
    ASSERT_TRUE(rw.Truncate(1024).ok());
    EXPECT_EQ(mock_meta_->truncate_calls, 1);
    EXPECT_EQ(mock_meta_->file_size(), 1024);
  });
}

TEST_F(FileReadWriterTest, TruncatePropagatesMetaError) {
  RunInTestFiber([&] {
    mock_meta_->set_truncate_status(Status::Internal("truncate failed"));
    auto rw = Make();
    auto status = rw.Truncate(1024);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), Status::kInternal);
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

TEST_F(FileReadWriterTest, TruncateQueuesDroppedChunkObjectsWithoutForegroundDelete) {
  // Truncate publishes cleanup candidates but never physically deletes from
  // the producer path. Reclaimer owns the authoritative revalidation gate.
  //
  // The default VolumeImpl chunk_size is 64 MiB, so seeding multiple
  // chunks via the high-level Write API would require writing >64 MiB
  // per chunk. Use the test-only chunk-size escape hatch on the volume
  // singleton to shrink it for the duration of this test.
  RunInTestFiber([&] {
    auto &vol = swordfs::volume::VolumeImpl::Instance();
    vol.set_chunk_size_for_test(kChunkSize);

    auto rw = Make();

    // Register three chunks with the mock metadata engine and data engine.
    for (ChunkIndex i = 0; i < 3; ++i) {
      SwordFsChunk chunk{};
      chunk.index = i;
      chunk.start_offset = i * kChunkSize;
      chunk.revision = static_cast<uint64_t>(i) + 1;
      chunk.size = kChunkSize;
      ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());
      auto buf = std::make_unique<folly::IOBuf>(Buf(Repeat('A', kChunkSize)));
      ASSERT_TRUE(
          mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(buf))
              .ok());
    }
    // Materialise each chunk in FileChunkManager by reading it; the
    // reader path uses FileChunkManager::Get in lookup-only mode, which triggers
    // Chunk::Initialize → meta->FindChunk → state kClean.
    for (ChunkIndex i = 0; i < 3; ++i) {
      auto out = folly::IOBuf::create(kChunkSize);
      ASSERT_TRUE(rw.Read(kChunkSize, i * kChunkSize, out.get()).ok());
    }
    ASSERT_EQ(mock_data_->StoredKeys().size(), 3);

    // Truncate to one byte — chunks 1 and 2 are dropped (their data
    // no longer fits the new size), chunk 0 is kept alive.
    ASSERT_TRUE(rw.Truncate(1).ok());
    EXPECT_TRUE(mock_data_->delete_calls.empty());
    EXPECT_EQ(mock_meta_->PendingDeleteKeys(),
              (std::vector<std::string>{swordfs::chunk::FormatChunkObjectKey(kIno, 1, 2),
                                        swordfs::chunk::FormatChunkObjectKey(kIno, 2, 3)}));
    EXPECT_EQ(mock_data_->StoredKeys().size(), 3);

    // Restore the production chunk size so a later test in this
    // fixture doesn't observe the override.
    vol.clear_chunk_size_for_test();
  });
}

TEST_F(FileReadWriterTest, ConcurrentReadsProceedWhileAnotherReadWaitsForBackend) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

  SwordFsChunk chunk{};
  chunk.index = 0;
  chunk.start_offset = 0;
  chunk.revision = 17;
  chunk.size = kChunkSize;
  ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());
  auto data = std::make_unique<folly::IOBuf>(Buf(Repeat('R', kChunkSize)));
  ASSERT_TRUE(
      mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(data)).ok());

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

  while (!second_get_started.try_wait() || !second_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(second_status.ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(second_out->data()), second_out->length()),
            Repeat('R', kChunkSize));

  release_first_get.post();
  while (!first_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(first_status.ok());
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(first_out->data()), first_out->length()),
            Repeat('R', kChunkSize));
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, IndependentChunkOverwriteHydrationDoesNotSerializeOnInode) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

  for (ChunkIndex index = 0; index < 2; ++index) {
    SwordFsChunk chunk{};
    chunk.index = index;
    chunk.start_offset = static_cast<uint64_t>(index) * kChunkSize;
    chunk.revision = 17 + index;
    chunk.size = kChunkSize;
    ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());
    auto data = std::make_unique<folly::IOBuf>(Buf(Repeat(index == 0 ? 'A' : 'B', kChunkSize)));
    ASSERT_TRUE(
        mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(data)).ok());
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

  while (!second_get_started.try_wait() || !second_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(second_status.ok());

  release_first_get.post();
  while (!first_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(first_status.ok());
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, WriteDuringBlockedFlushDoesNotWaitForRemotePut) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

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
  fm.addTask([&] {
    flush_status = rw.Flush();
    flush_done.post();
  });
  fm.addTask([&] {
    put_started.wait();
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

  while (!probe_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(write_done.try_wait()) << "foreground write waited for the older generation's remote Put";

  release_put.post();
  while (!flush_done.try_wait() || !write_done.try_wait()) {
    evb.loopOnce();
  }
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
    FileReadWriter reopened(kIno);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, WriteDuringBlockedCommitDoesNotWaitForRemotePublication) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

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

  while (!probe_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(write_done.try_wait()) << "foreground write waited for the older generation's metadata commit";

  release_commit.post();
  while (!flush_done.try_wait() || !write_done.try_wait()) {
    evb.loopOnce();
  }
  ASSERT_TRUE(flush_status.ok());
  ASSERT_TRUE(write_status.ok());

  RunInTestFiber([&] {
    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(rw.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "AAAABBBB");

    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, FailedOlderPutPreservesRepeatedWritesDuringFlush) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

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

  while (!writes_done.try_wait()) {
    evb.loopOnce();
  }
  ASSERT_TRUE(first_write_status.ok());
  ASSERT_TRUE(second_write_status.ok());
  release_put.post();
  while (!flush_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_FALSE(flush_status.ok());

  RunInTestFiber([&] {
    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(rw.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "ACCABBBB");

    mock_data_->put_status = Status::OK();
    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "ACCABBBB");
  });
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, WriteDuringAmbiguousCommitPreservesLatestLocalGeneration) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

  auto rw = Make();
  RunInTestFiber([&] { ASSERT_TRUE(rw.Write(Buf("AAAA"), 0).ok()); });

  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton commit_started;
  folly::fibers::Baton release_commit;
  folly::fibers::Baton write_done;
  folly::fibers::Baton flush_done;
  mock_meta_->publish_chunk_status = Status::IOError("response lost after commit");
  mock_meta_->publish_chunk_commit_on_error = true;
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

  while (!write_done.try_wait()) {
    evb.loopOnce();
  }
  ASSERT_TRUE(write_status.ok());
  release_commit.post();
  while (!flush_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_FALSE(flush_status.ok());

  RunInTestFiber([&] {
    auto local = folly::IOBuf::create(8);
    ASSERT_TRUE(rw.Read(8, 0, local.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(local->data()), local->length()), "AAAABBBB");

    mock_meta_->publish_chunk_status = Status::OK();
    mock_meta_->publish_chunk_commit_on_error = false;
    ASSERT_TRUE(rw.Flush().ok());
    FileReadWriter reopened(kIno);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, ConcurrentFlushBarriersPublishGenerationsInOrder) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

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

  while (!probe_done.try_wait()) {
    evb.loopOnce();
  }
  ASSERT_TRUE(write_status.ok());
  EXPECT_EQ(mock_data_->put_calls, 1) << "a newer generation began publication before the older generation completed";

  release_first_put.post();
  while (!first_flush_done.try_wait() || !second_flush_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(first_flush_status.ok());
  EXPECT_TRUE(second_flush_status.ok());
  EXPECT_EQ(mock_data_->put_calls, 2);

  RunInTestFiber([&] {
    FileReadWriter reopened(kIno);
    auto persisted = folly::IOBuf::create(8);
    ASSERT_TRUE(reopened.Read(8, 0, persisted.get()).ok());
    EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(persisted->data()), persisted->length()), "AAAABBBB");
  });
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, MultiChunkFlushStartsAnotherPutWhileFirstPutIsBlocked) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

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

  while (!probe_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(second_put_started.try_wait()) << "one chunk's remote Put serialized every chunk in the inode";

  release_first_put.post();
  while (!flush_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(flush_status.ok());
  EXPECT_EQ(mock_data_->put_calls, 2);
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, TruncateWaitsForBlockedFlushPublication) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

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

  while (!put_started.try_wait() || !truncate_started.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_EQ(mock_meta_->truncate_calls, 0) << "truncate raced an older publication and could be republished over EOF";

  release_put.post();
  while (!flush_done.try_wait() || !truncate_done.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_TRUE(flush_status.ok());
  EXPECT_TRUE(truncate_status.ok());
  EXPECT_EQ(mock_meta_->truncate_calls, 1);
  vol.clear_chunk_size_for_test();
}

TEST_F(FileReadWriterTest, TruncateWaitsForBlockedReadWithoutBlockingEventBase) {
  auto &vol = swordfs::volume::VolumeImpl::Instance();
  vol.set_chunk_size_for_test(kChunkSize);

  SwordFsChunk chunk{};
  chunk.index = 0;
  chunk.start_offset = 0;
  chunk.revision = 17;
  chunk.size = kChunkSize;
  ASSERT_TRUE(mock_meta_->SeedChunkForTest(kIno, chunk).ok());
  auto data = std::make_unique<folly::IOBuf>(Buf(Repeat('R', kChunkSize)));
  ASSERT_TRUE(
      mock_data_->Put(swordfs::chunk::FormatChunkObjectKey(kIno, chunk.index, chunk.revision), std::move(data)).ok());

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

  while (!get_started.try_wait() || !truncate_started.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_EQ(mock_meta_->truncate_calls, 0);

  release_get.post();
  while (!read_done.try_wait() || !truncate_done.try_wait()) {
    evb.loopOnce();
  }

  EXPECT_TRUE(read_status.ok());
  EXPECT_TRUE(truncate_status.ok());
  EXPECT_EQ(mock_meta_->truncate_calls, 1);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char *>(out->data()), out->length()), Repeat('R', kChunkSize));
  vol.clear_chunk_size_for_test();
}
