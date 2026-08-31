// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "metadata/IMetaEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/DirHandle.hpp"

namespace swordfs::vfs {
namespace {

using metadata::DirIterator;
using metadata::DirIteratorPtr;
using metadata::SwordFsEntry;
using utils::Status;

class TestDirIterator final : public DirIterator {
 public:
  explicit TestDirIterator(std::vector<SwordFsEntry> entries) : entries_(std::move(entries)) {
  }

  Status Seek(uint64_t cookie) override {
    position_ = cookie;
    pending_next_.reset();
    return Status::OK();
  }

  Status Peek(SwordFsEntry *entry, uint64_t *next_cookie) override {
    if (position_ >= entries_.size()) {
      return Status::EndOfDirectory("end");
    }
    *entry = entries_[position_];
    *next_cookie = position_ + 1;
    pending_next_ = *next_cookie;
    return Status::OK();
  }

  void Advance() override {
    ASSERT_TRUE(pending_next_.has_value());
    position_ = *pending_next_;
    pending_next_.reset();
  }

 private:
  std::vector<SwordFsEntry> entries_;
  uint64_t position_ = 0;
  std::optional<uint64_t> pending_next_;
};

class TestDirEntryEncoder final : public DirEntryEncoder {
 public:
  size_t CalSpace(const SwordFsEntry &entry, off_t next_off) const override {
    return Encoded(entry, next_off).size();
  }

  void Encode(const SwordFsEntry &entry, off_t next_off, size_t required, std::string *out) const override {
    const std::string encoded = Encoded(entry, next_off);
    ASSERT_EQ(encoded.size(), required);
    out->append(encoded);
  }

 private:
  static std::string Encoded(const SwordFsEntry &entry, off_t next_off) {
    return entry.name + ":" + std::to_string(entry.ino) + ":" + std::to_string(next_off);
  }
};

const TestDirEntryEncoder &Encoder() {
  static const TestDirEntryEncoder encoder;
  return encoder;
}

TEST(DirHandleTest, ReadDirAcceptsZeroSize) {
  auto handle = std::make_shared<DirHandle>(std::make_shared<TestDirIterator>(std::vector<SwordFsEntry>{}));
  std::string output = "stale";

  ASSERT_TRUE(handle->ReadDir(0, 0, Encoder(), &output).ok());
  EXPECT_TRUE(output.empty());
}

TEST(DirHandleTest, ReadDirEncodesEntriesAndAdvancesOffset) {
  auto handle = std::make_shared<DirHandle>(
      std::make_shared<TestDirIterator>(std::vector<SwordFsEntry>{{"one", DT_REG, 10}, {"two", DT_DIR, 20}}));

  std::string output = "stale";
  ASSERT_TRUE(handle->ReadDir(0, 100, Encoder(), &output).ok());
  EXPECT_EQ(output, "one:10:1two:20:2");
}

TEST(DirHandleTest, ReadDirResumesFromOffset) {
  auto handle = std::make_shared<DirHandle>(
      std::make_shared<TestDirIterator>(std::vector<SwordFsEntry>{{"one", DT_REG, 10}, {"two", DT_DIR, 20}}));

  std::string output;
  ASSERT_TRUE(handle->ReadDir(1, 100, Encoder(), &output).ok());
  EXPECT_EQ(output, "two:20:2");
}

TEST(DirHandleTest, ReadDirStopsWhenEntryDoesNotFit) {
  auto handle =
      std::make_shared<DirHandle>(std::make_shared<TestDirIterator>(std::vector<SwordFsEntry>{{"one", DT_REG, 10}}));

  std::string output;
  auto status = handle->ReadDir(0, 3, Encoder(), &output);
  EXPECT_EQ(status.code(), Status::kNoMemory);
  EXPECT_TRUE(output.empty());
}

TEST(DirHandleTest, ReadDirReturnsEndOfDirectoryAsSuccess) {
  auto handle = std::make_shared<DirHandle>(std::make_shared<TestDirIterator>(std::vector<SwordFsEntry>{}));

  std::string output = "stale";
  ASSERT_TRUE(handle->ReadDir(0, 100, Encoder(), &output).ok());
  EXPECT_TRUE(output.empty());
}

}  // namespace
}  // namespace swordfs::vfs
