// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>

#include "metadata/Utils.hpp"

using swordfs::metadata::MakeStat;
using swordfs::metadata::ModeToDt;

TEST(UtilsTest, ModeToDtReg) {
  EXPECT_EQ(ModeToDt(S_IFREG | 0644), DT_REG);
}

TEST(UtilsTest, ModeToDtDir) {
  EXPECT_EQ(ModeToDt(S_IFDIR | 0755), DT_DIR);
}

TEST(UtilsTest, ModeToDtLnk) {
  EXPECT_EQ(ModeToDt(S_IFLNK | 0777), DT_LNK);
}

TEST(UtilsTest, ModeToDtBlk) {
  EXPECT_EQ(ModeToDt(S_IFBLK | 0600), DT_BLK);
}

TEST(UtilsTest, ModeToDtChr) {
  EXPECT_EQ(ModeToDt(S_IFCHR | 0600), DT_CHR);
}

TEST(UtilsTest, ModeToDtFifo) {
  EXPECT_EQ(ModeToDt(S_IFIFO | 0600), DT_FIFO);
}

TEST(UtilsTest, ModeToDtSock) {
  EXPECT_EQ(ModeToDt(S_IFSOCK | 0600), DT_SOCK);
}

TEST(UtilsTest, MakeStatSetsMode) {
  struct stat st = MakeStat(S_IFREG | 0644, 12345);
  EXPECT_EQ(st.st_mode & S_IFMT, S_IFREG);
  EXPECT_EQ(st.st_mode & 07777, 0644);
}

TEST(UtilsTest, MakeStatDirectoryHasNlink2) {
  struct stat st = MakeStat(S_IFDIR | 0755, 12345);
  EXPECT_EQ(st.st_nlink, 2);
}

TEST(UtilsTest, MakeStatRegularFileHasNlink1) {
  struct stat st = MakeStat(S_IFREG | 0644, 12345);
  EXPECT_EQ(st.st_nlink, 1);
}

TEST(UtilsTest, MakeStatSetsTimestamps) {
  struct stat st = MakeStat(S_IFREG | 0644, 12345);
  EXPECT_EQ(st.st_atime, 12345);
  EXPECT_EQ(st.st_mtime, 12345);
  EXPECT_EQ(st.st_ctime, 12345);
}
