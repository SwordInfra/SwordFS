// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end persistence tests. Each case crosses a full SwordFS daemon
// lifecycle so post-remount assertions observe Redis + object-store state,
// not the previous client's in-memory caches.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <string>

#include "tests/e2e/Fixture.hpp"
#include "tests/e2e/Utils.hpp"

using swordfs::e2e::Fixture;
using swordfs::e2e::MakeDeterministicPayload;

class PersistenceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp());
  }

  void TearDown() override {
    fixture_.TearDown();
  }

  Fixture fixture_;
};

TEST_F(PersistenceTest, FullChunkAndSecondChunkDataPersistAcrossRemount) {
  constexpr size_t kChunkSize = 64ULL * 1024 * 1024;
  constexpr size_t kSecondChunkBytes = 16ULL * 1024 * 1024;
  const std::string name = "multi_chunk_persist.bin";

  // A position-sensitive binary pattern makes the whole-file hash detect
  // wrong-object, chunk-ordering, repeated-block, and ordinary corruption
  // bugs. This is stronger than filling the 80 MiB file with one byte value.
  const std::string data = MakeDeterministicPayload(kChunkSize + kSecondChunkBytes);

  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, data), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, static_cast<off_t>(data.size()));
  EXPECT_TRUE(fixture_.FileEquals(name, data.size(), Fixture::Hash64(data)));

  int fd = fixture_.OpenFile(name, O_RDONLY);
  ASSERT_GE(fd, 0);
  std::string before_boundary(16, '\0');
  std::string after_boundary(16, '\0');
  ASSERT_EQ(::pread(fd, before_boundary.data(), before_boundary.size(), kChunkSize - before_boundary.size()),
            static_cast<ssize_t>(before_boundary.size()));
  ASSERT_EQ(::pread(fd, after_boundary.data(), after_boundary.size(), kChunkSize),
            static_cast<ssize_t>(after_boundary.size()));
  ASSERT_EQ(::close(fd), 0);
  EXPECT_EQ(before_boundary, data.substr(kChunkSize - before_boundary.size(), before_boundary.size()));
  EXPECT_EQ(after_boundary, data.substr(kChunkSize, after_boundary.size()));
}

TEST_F(PersistenceTest, FsyncPersistsDataAndSizeAcrossRemount) {
  const std::string name = "fsync_persist.bin";
  const std::string payload = "fsync persistence payload";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR | O_TRUNC), 0);

  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, payload.data(), payload.size(), 0), static_cast<ssize_t>(payload.size()));
  ASSERT_EQ(::fsync(fd), 0);
  ASSERT_EQ(::close(fd), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, static_cast<off_t>(payload.size()));
  EXPECT_TRUE(fixture_.FileEquals(name, payload.size(), Fixture::Hash64(payload)));
}

TEST_F(PersistenceTest, FsyncPersistsDataAndSizeAcrossDaemonCrash) {
  const std::string name = "fsync_crash_persist.bin";
  const std::string payload = "fsync crash persistence payload";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR | O_TRUNC), 0);

  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, payload.data(), payload.size(), 0), static_cast<ssize_t>(payload.size()));
  ASSERT_EQ(::fsync(fd), 0);
  ASSERT_EQ(::close(fd), 0);

  ASSERT_TRUE(fixture_.CrashAndRemount());

  struct stat st{};
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, static_cast<off_t>(payload.size()));
  EXPECT_TRUE(fixture_.FileEquals(name, payload.size(), Fixture::Hash64(payload)));
}

TEST_F(PersistenceTest, SparseWritePersistsZeroFilledHoleAcrossRemount) {
  const std::string name = "sparse_persist.bin";
  constexpr off_t kTailOffset = 4096;
  const std::string tail = "tail";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR | O_TRUNC), 0);

  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, tail.data(), tail.size(), kTailOffset), static_cast<ssize_t>(tail.size()));
  ASSERT_EQ(::fsync(fd), 0);
  ASSERT_EQ(::close(fd), 0);

  ASSERT_TRUE(fixture_.Remount());

  std::string content;
  ASSERT_EQ(fixture_.ReadFile(name, &content), 0);
  ASSERT_EQ(content.size(), static_cast<size_t>(kTailOffset) + tail.size());
  EXPECT_EQ(content.substr(0, kTailOffset), std::string(kTailOffset, '\0'));
  EXPECT_EQ(content.substr(kTailOffset), tail);
}

TEST_F(PersistenceTest, TruncateShrinkPersistsAcrossRemount) {
  const std::string name = "truncate_shrink.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hello world"), 0);
  ASSERT_EQ(fixture_.Truncate(name, 5), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, 5);
  EXPECT_TRUE(fixture_.FileEquals(name, 5, Fixture::Hash64("hello")));
}

TEST_F(PersistenceTest, TruncateExtendPersistsZeroFillAcrossRemount) {
  const std::string name = "truncate_extend.bin";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "hi"), 0);
  ASSERT_EQ(fixture_.Truncate(name, 10), 0);

  ASSERT_TRUE(fixture_.Remount());

  std::string content;
  ASSERT_EQ(fixture_.ReadFile(name, &content), 0);
  ASSERT_EQ(content.size(), 10U);
  EXPECT_EQ(content.substr(0, 2), "hi");
  EXPECT_EQ(content.substr(2), std::string(8, '\0'));
}

TEST_F(PersistenceTest, CrossChunkTruncatePersistsPrunedMetadataAcrossRemount) {
  const std::string name = "truncate_cross_chunk.bin";
  constexpr off_t kChunkSize = 64ULL * 1024 * 1024;
  const std::string head = "abcdef";
  const std::string tail = "second-chunk";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_RDWR | O_TRUNC), 0);

  int fd = fixture_.OpenFile(name, O_RDWR);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::pwrite(fd, head.data(), head.size(), 0), static_cast<ssize_t>(head.size()));
  ASSERT_EQ(::pwrite(fd, tail.data(), tail.size(), kChunkSize), static_cast<ssize_t>(tail.size()));
  ASSERT_EQ(::fsync(fd), 0);
  ASSERT_EQ(::close(fd), 0);
  ASSERT_EQ(fixture_.Truncate(name, 3), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_size, 3);
  EXPECT_TRUE(fixture_.FileEquals(name, 3, Fixture::Hash64("abc")));

  // Re-extending past the old second chunk must not resurrect the chunk that
  // truncate removed before the remount.
  ASSERT_EQ(fixture_.Truncate(name, kChunkSize + tail.size()), 0);
  fd = fixture_.OpenFile(name, O_RDONLY);
  ASSERT_GE(fd, 0);
  std::string zeros(tail.size(), '\0');
  std::string observed(tail.size(), '\0');
  ASSERT_EQ(::pread(fd, observed.data(), observed.size(), kChunkSize), static_cast<ssize_t>(observed.size()));
  ASSERT_EQ(::close(fd), 0);
  EXPECT_EQ(observed, zeros);
}

TEST_F(PersistenceTest, RenameOverwritePersistsAcrossRemount) {
  const std::string source = "rename_source.txt";
  const std::string target = "rename_target.txt";
  ASSERT_EQ(fixture_.CreateFile(source, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.CreateFile(target, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(source, "replacement"), 0);
  ASSERT_EQ(fixture_.WriteFile(target, "old"), 0);
  ASSERT_EQ(fixture_.Rename(source, target), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  errno = 0;
  EXPECT_EQ(fixture_.Stat(source, &st), -1);
  EXPECT_EQ(errno, ENOENT);
  EXPECT_TRUE(fixture_.FileEquals(target, 11, Fixture::Hash64("replacement")));
}

TEST_F(PersistenceTest, DirectoryRenameAcrossParentsPersistsAcrossRemount) {
  ASSERT_EQ(fixture_.MkDir("left", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("right", 0755), 0);
  ASSERT_EQ(fixture_.MkDir("left/child", 0755), 0);
  ASSERT_EQ(fixture_.CreateFile("left/child/data.txt", 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile("left/child/data.txt", "nested"), 0);
  ASSERT_EQ(fixture_.Rename("left/child", "right/child"), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  errno = 0;
  EXPECT_EQ(fixture_.Stat("left/child", &st), -1);
  EXPECT_EQ(errno, ENOENT);
  ASSERT_EQ(fixture_.Stat("right/child", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_TRUE(fixture_.FileEquals("right/child/data.txt", 6, Fixture::Hash64("nested")));

  struct stat left_st{};
  struct stat right_st{};
  ASSERT_EQ(fixture_.Stat("left", &left_st), 0);
  ASSERT_EQ(fixture_.Stat("right", &right_st), 0);
  EXPECT_EQ(left_st.st_nlink, 2U);
  EXPECT_EQ(right_st.st_nlink, 3U);
}

TEST_F(PersistenceTest, NamespaceDeletionPersistsAcrossRemount) {
  const std::string file = "deleted.txt";
  const std::string dir = "deleted_dir";
  ASSERT_EQ(fixture_.CreateFile(file, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(file, "gone"), 0);
  ASSERT_EQ(fixture_.MkDir(dir, 0755), 0);
  ASSERT_EQ(fixture_.UnlinkFile(file), 0);
  ASSERT_EQ(fixture_.RmDir(dir), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  errno = 0;
  EXPECT_EQ(fixture_.Stat(file, &st), -1);
  EXPECT_EQ(errno, ENOENT);
  errno = 0;
  EXPECT_EQ(fixture_.Stat(dir, &st), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(PersistenceTest, HardLinkIdentityPersistsAcrossRemount) {
  const std::string original = "hardlink_original.txt";
  const std::string link = "hardlink_survivor.txt";
  const std::string payload = "hardlink-data";
  ASSERT_EQ(fixture_.CreateFile(original, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(original, payload), 0);

  struct stat original_st{};
  ASSERT_EQ(fixture_.Stat(original, &original_st), 0);
  ASSERT_EQ(fixture_.HardLink(original, link), 0);
  ASSERT_EQ(fixture_.UnlinkFile(original), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat link_st{};
  ASSERT_EQ(fixture_.Stat(link, &link_st), 0);
  EXPECT_EQ(link_st.st_ino, original_st.st_ino);
  EXPECT_EQ(link_st.st_nlink, 1U);
  EXPECT_TRUE(fixture_.FileEquals(link, payload.size(), Fixture::Hash64(payload)));
}

TEST_F(PersistenceTest, SymlinkPayloadPersistsAcrossRemount) {
  const std::string target = "symlink_target.txt";
  const std::string link = "symlink_persist";
  const std::string payload = "target-data";
  ASSERT_EQ(fixture_.CreateFile(target, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(target, payload), 0);
  ASSERT_EQ(fixture_.Symlink(target, link), 0);

  ASSERT_TRUE(fixture_.Remount());

  std::string stored_target;
  ASSERT_EQ(fixture_.Readlink(link, &stored_target), 0);
  EXPECT_EQ(stored_target, target);
  struct stat st{};
  ASSERT_EQ(fixture_.Lstat(link, &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));
  EXPECT_TRUE(fixture_.FileEquals(link, payload.size(), Fixture::Hash64(payload)));
}

TEST_F(PersistenceTest, ChmodPersistsAcrossRemount) {
  const std::string name = "chmod_persist.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.Chmod(name, 0600), 0);

  ASSERT_TRUE(fixture_.Remount());

  struct stat st{};
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0600);
}
