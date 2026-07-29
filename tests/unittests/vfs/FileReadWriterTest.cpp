// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cstring>

#include "chunk/Chunk.hpp"
#include "metadata/Meta.hpp"
#include "storage/IDataEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/FileReadWriter.hpp"

using swordfs::chunk::Chunk;
using swordfs::metadata::Meta;
using swordfs::metadata::InodeID;
using swordfs::storage::DataEngineLimits;
using swordfs::storage::IDataEngine;
using swordfs::utils::Status;
using swordfs::vfs::FileReadWriter;

namespace {

// Minimal mock implementations.

class MockDataEngine : public IDataEngine {
 public:
  DataEngineLimits Limits() const override { return DataEngineLimits{}; }
  bool Head(std::string_view /*key*/, size_t* size) override {
    if (size) *size = 0;
    return false;
  }
  Status Put(std::string_view /*key*/, std::string_view /*data*/) override {
    return Status::OK();
  }
  Status Get(std::string_view /*key*/, std::string* out, size_t, size_t) override {
    return Status::NotFound("not found");
  }
  Status Delete(std::string_view /*key*/) override { return Status::OK(); }
};

class MockMetaEngine : public Meta {
 public:
  Status GetAttr(InodeID, struct stat* attr) override {
    std::memset(attr, 0, sizeof(*attr));
    attr->st_size = 0;
    return Status::OK();
  }
  Status SetAttr(InodeID, const struct stat*, int, struct stat*) override {
    return Status::OK();
  }
  Status Lookup(InodeID, std::string_view, InodeID*, struct stat*) override { return Status::NotFound(""); }
  Status ReadDir(InodeID, std::vector<swordfs::metadata::SwordFsEntry>*) override { return Status::OK(); }
  Status Create(InodeID, std::string_view, mode_t, InodeID*, struct stat*) override { return Status::OK(); }
  Status MkDir(InodeID, std::string_view, mode_t, InodeID*, struct stat*) override { return Status::OK(); }
  Status Unlink(InodeID, std::string_view) override { return Status::OK(); }
  Status RmDir(InodeID, std::string_view) override { return Status::OK(); }
  Status Rename(InodeID, std::string_view, InodeID, std::string_view, unsigned int) override { return Status::OK(); }
  Status Access(InodeID, int) override { return Status::OK(); }
  Status Open(InodeID, uint64_t*) override { return Status::OK(); }
  Status Release(uint64_t) override { return Status::OK(); }
  Status OpenDir(InodeID, uint64_t*) override { return Status::OK(); }
  Status ReleaseDir(uint64_t) override { return Status::OK(); }
  Status Forget(InodeID, uint64_t) override { return Status::OK(); }
  Status StatFs(struct statvfs* st) override {
    std::memset(st, 0, sizeof(*st));
    return Status::OK();
  }
  Status AppendSlice(InodeID, uint64_t, const swordfs::storage::Slice&) override { return Status::OK(); }
  Status GetSlices(InodeID, uint64_t, swordfs::storage::SliceList*) override { return Status::OK(); }
  uint64_t NextSliceID(InodeID) override { return 0; }
};

static constexpr InodeID kTestIno = 42;

class FileReadWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    meta_ = std::make_unique<MockMetaEngine>();
    data_ = std::make_unique<MockDataEngine>();
  }

  FileReadWriter MakeWriter() {
    return FileReadWriter(meta_.get(), data_.get(), kTestIno);
  }

  std::unique_ptr<MockMetaEngine> meta_;
  std::unique_ptr<MockDataEngine> data_;
};

// ── Constructor ────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, Construct) {
  FileReadWriter fw(meta_.get(), data_.get(), kTestIno);
  // Just verify construction succeeds.
}

// ── Write ───────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, WriteBasic) {
  auto fw = MakeWriter();
  const char* msg = "hello";
  Status st = fw.Write(msg, 5, 0);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(FileReadWriterTest, WriteLargeSplitsChunks) {
  auto fw = MakeWriter();
  size_t max_chunk = data_->Limits().max_chunk_size;
  std::string big(max_chunk + 1024, 'X');
  Status st = fw.Write(big.data(), big.size(), 0);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(FileReadWriterTest, WriteNoDataEngine) {
  FileReadWriter fw(meta_.get(), nullptr, kTestIno);
  Status st = fw.Write("x", 1, 0);
  EXPECT_FALSE(st.ok());
}

// ── Read ────────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, ReadBasic) {
  auto fw = MakeWriter();
  folly::IOBuf out(folly::IOBuf::CREATE, 1024);
  Status st = fw.Read(10, 0, &out);
  // Read from storage with no data → OK but 0 bytes.
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(FileReadWriterTest, ReadNoDataEngine) {
  FileReadWriter fw(meta_.get(), nullptr, kTestIno);
  folly::IOBuf out(folly::IOBuf::CREATE, 1024);
  Status st = fw.Read(10, 0, &out);
  EXPECT_FALSE(st.ok());
}

// ── FindChunk ───────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, FindChunkWrongIno) {
  auto fw = MakeWriter();
  EXPECT_EQ(fw.FindChunk(999, 0), nullptr);
}

TEST_F(FileReadWriterTest, FindChunkNoMatch) {
  auto fw = MakeWriter();
  EXPECT_EQ(fw.FindChunk(kTestIno, 0), nullptr);  // no chunks written
}

// ── Flush ───────────────────────────────────────────────────────────

TEST_F(FileReadWriterTest, FlushEmpty) {
  auto fw = MakeWriter();
  Status st = fw.Flush();
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(FileReadWriterTest, FlushAfterWrite) {
  auto fw = MakeWriter();
  ASSERT_TRUE(fw.Write("test", 4, 0).ok());
  Status st = fw.Flush();
  EXPECT_TRUE(st.ok()) << st.message();
}
