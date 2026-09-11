// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
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

class BlockingDirIterator final : public DirIterator {
 public:
  BlockingDirIterator(folly::fibers::Baton *peek_entered, folly::fibers::Baton *release_peek)
      : peek_entered_(peek_entered), release_peek_(release_peek) {
  }

  Status Seek(uint64_t cookie) override {
    ++seek_calls_;
    position_ = cookie;
    return Status::OK();
  }

  Status Peek(SwordFsEntry *entry, uint64_t *next_cookie) override {
    if (!blocked_once_) {
      blocked_once_ = true;
      peek_entered_->post();
      release_peek_->wait();
    }
    if (position_ >= 2) {
      return Status::EndOfDirectory("end");
    }
    *entry = position_ == 0 ? SwordFsEntry{"one", DT_REG, 10} : SwordFsEntry{"two", DT_REG, 20};
    *next_cookie = position_ + 1;
    pending_next_ = *next_cookie;
    return Status::OK();
  }

  void Advance() override {
    ASSERT_TRUE(pending_next_.has_value());
    position_ = *pending_next_;
    pending_next_.reset();
  }

  int seek_calls() const {
    return seek_calls_;
  }

 private:
  folly::fibers::Baton *peek_entered_;
  folly::fibers::Baton *release_peek_;
  uint64_t position_{0};
  std::optional<uint64_t> pending_next_;
  int seek_calls_{0};
  bool blocked_once_{false};
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

TEST(DirHandleTest, ConcurrentReadDirSerializesWholeIteratorOperation) {
  folly::EventBase evb;
  auto &fm = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton peek_entered;
  folly::fibers::Baton release_peek;
  folly::fibers::Baton second_started;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;

  auto iterator = std::make_shared<BlockingDirIterator>(&peek_entered, &release_peek);
  auto handle = std::make_shared<DirHandle>(iterator);
  std::string first_output;
  std::string second_output;
  Status first_status;
  Status second_status;

  fm.addTask([&] {
    first_status = handle->ReadDir(0, 100, Encoder(), &first_output);
    first_done.post();
  });
  fm.addTask([&] {
    peek_entered.wait();
    second_started.post();
    second_status = handle->ReadDir(1, 100, Encoder(), &second_output);
    second_done.post();
  });

  while (!peek_entered.try_wait() || !second_started.try_wait()) {
    evb.loopOnce();
  }
  EXPECT_EQ(iterator->seek_calls(), 1);

  release_peek.post();
  while (!first_done.try_wait() || !second_done.try_wait()) {
    evb.loopOnce();
  }

  EXPECT_TRUE(first_status.ok());
  EXPECT_TRUE(second_status.ok());
  EXPECT_EQ(iterator->seek_calls(), 2);
  EXPECT_EQ(first_output, "one:10:1two:20:2");
  EXPECT_EQ(second_output, "two:20:2");
}

}  // namespace
}  // namespace swordfs::vfs
