// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include <set>

#include "metadata/mem/MemMetaImpl.hpp"
#include "utils/Context.hpp"
#include "utils/Status.hpp"

using swordfs::metadata::InodeID;
using swordfs::metadata::MemMetaImpl;
using swordfs::metadata::RenameFlag;
using swordfs::metadata::SwordFsEntry;
using swordfs::metadata::SwordFsInode;
using swordfs::utils::Status;
using swordfs::utils::SwordFsContext;

static constexpr InodeID kRoot = swordfs::metadata::kRootInodeId;

class MemMetaImplReadDirTest : public ::testing::Test {
 protected:
  void SetUp() override {
    impl_ = new MemMetaImpl();
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
  }
  void TearDown() override { delete impl_; }

  MemMetaImpl *impl_;
};

TEST_F(MemMetaImplReadDirTest, ReadDirEmpty) {
  std::vector<SwordFsEntry> entries;
  Status st = impl_->ReadDir(kRoot, &entries);
  EXPECT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].name, ".");
  EXPECT_EQ(entries[0].ino, kRoot);
  EXPECT_EQ(entries[1].name, "..");
  EXPECT_EQ(entries[1].ino, kRoot);
}

TEST_F(MemMetaImplReadDirTest, ReadDirWithEntries) {
  constexpr int kFiles = 10;
  for (int i = 0; i < kFiles; ++i) {
    SwordFsInode inode;
    std::string name = "file_" + std::to_string(i);
    ASSERT_TRUE(impl_->Create(kRoot, name, 0644, &inode).ok());
  }
  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(impl_->ReadDir(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), kFiles + 2);
  std::set<std::string> names;
  for (const auto &e : entries) {
    EXPECT_TRUE(names.insert(e.name).second) << "Duplicate entry: " << e.name;
    if (e.name == "." || e.name == "..") continue;
    EXPECT_GT(e.ino, kRoot);
    EXPECT_EQ(e.type, DT_REG);
  }
}

TEST_F(MemMetaImplReadDirTest, ReadDirNotADirectory) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "regular", 0644, &file).ok());
  std::vector<SwordFsEntry> entries;
  Status st = impl_->ReadDir(file.ino, &entries);
  EXPECT_TRUE(st.IsNotDirectory()) << st.message();
}

TEST_F(MemMetaImplReadDirTest, ReadDirMixedTypes) {
  SwordFsInode file, dir;
  ASSERT_TRUE(impl_->Create(kRoot, "file.txt", 0644, &file).ok());
  ASSERT_TRUE(impl_->MkDir(kRoot, "subdir", 0755, &dir).ok());
  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(impl_->ReadDir(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), 4);
  for (const auto &e : entries) {
    if (e.name == "file.txt") EXPECT_EQ(e.type, DT_REG);
    else if (e.name == "subdir" || e.name == "." || e.name == "..") EXPECT_EQ(e.type, DT_DIR);
    else FAIL() << "Unexpected entry: " << e.name;
  }
}

TEST_F(MemMetaImplReadDirTest, ReadDirAfterMove) {
  SwordFsInode dir_a, dir_b, file;
  ASSERT_TRUE(impl_->MkDir(kRoot, "a", 0755, &dir_a).ok());
  ASSERT_TRUE(impl_->MkDir(kRoot, "b", 0755, &dir_b).ok());
  ASSERT_TRUE(impl_->Create(dir_a.ino, "target", 0644, &file).ok());
  ASSERT_TRUE(impl_->Rename(dir_a.ino, "target", dir_b.ino, "target", RenameFlag::kNone).ok());
  std::vector<SwordFsEntry> entries_a, entries_b;
  impl_->ReadDir(dir_a.ino, &entries_a);
  impl_->ReadDir(dir_b.ino, &entries_b);
  EXPECT_EQ(entries_a.size(), 2);
  EXPECT_EQ(entries_b.size(), 3);
  const SwordFsEntry *target_entry = nullptr;
  for (const auto &e : entries_b) if (e.name != "." && e.name != "..") { target_entry = &e; break; }
  ASSERT_NE(target_entry, nullptr);
  EXPECT_EQ(target_entry->name, "target");
  EXPECT_EQ(target_entry->ino, file.ino);
}

TEST_F(MemMetaImplReadDirTest, ReadDirAfterUnlink) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRoot, "to_delete", 0644, &file).ok());
  impl_->Unlink(kRoot, "to_delete");
  std::vector<SwordFsEntry> entries;
  impl_->ReadDir(kRoot, &entries);
  EXPECT_EQ(entries.size(), 2);
}

TEST_F(MemMetaImplReadDirTest, ReadDirLargeDirectory) {
  constexpr int kFiles = 200;
  for (int i = 0; i < kFiles; ++i) {
    SwordFsInode inode;
    ASSERT_TRUE(impl_->Create(kRoot, "entry_" + std::to_string(i), 0644, &inode).ok());
  }
  std::vector<SwordFsEntry> entries;
  EXPECT_TRUE(impl_->ReadDir(kRoot, &entries).ok());
  EXPECT_EQ(entries.size(), kFiles + 2);
  std::set<InodeID> inodes;
  for (const auto &e : entries) if (e.name != "." && e.name != "..") inodes.insert(e.ino);
  EXPECT_EQ(inodes.size(), kFiles) << "Duplicate inodes in ReadDir output";
}
