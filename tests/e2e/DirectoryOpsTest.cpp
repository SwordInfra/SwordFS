// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: directory operations.
//
// Validates: mkdir, rmdir, readdir, truncate-on-dir,
//            directory name length limits.

#include <dirent.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

#include "tests/e2e/Const.hpp"
#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;
using namespace swordfs::e2e;

class DirectoryOpsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp()) << "Failed to set up E2E fixture";
  }
  void TearDown() override {
    fixture_.TearDown();
  }
  Fixture fixture_;
};

// ────────────────────────────────────────────────────────────────
// mkdir / rmdir — happy path
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirAndRmdir) {
  const char *name = "subdir";
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  ASSERT_EQ(fixture_.RmDir(name), 0);
  EXPECT_NE(fixture_.Stat(name, &st), 0);
}

TEST_F(DirectoryOpsTest, MkdirNested) {
  ASSERT_EQ(fixture_.MkDir("a", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir("a/b", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir("a/b/c", kDefaultDirMode), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat("a/b/c", &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
}

TEST_F(DirectoryOpsTest, StatDir) {
  const std::string name = "d";
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), 0);
  struct stat st;
  ASSERT_EQ(fixture_.Stat(name, &st), 0);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(st.st_size, 4096);
  EXPECT_EQ(st.st_nlink, static_cast<nlink_t>(2));
  EXPECT_TRUE(fixture_.UmaskEquals(name, kDefaultDirMode));
}

// ────────────────────────────────────────────────────────────────
// mkdir — error cases
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirExisting) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), -1);
  EXPECT_EQ(errno, EEXIST);
}

TEST_F(DirectoryOpsTest, MkdirUnderFile) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  ASSERT_EQ(fixture_.MkDir(name + std::string("/sub"), kDefaultDirMode), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, MkdirUnderNonexistent) {
  ASSERT_EQ(fixture_.MkDir("noent/sub", kDefaultDirMode), -1);
  EXPECT_EQ(errno, ENOENT);
}

// ────────────────────────────────────────────────────────────────
// rmdir — error cases
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RmdirNonEmpty) {
  const char *name = "d";
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile(name + std::string("/f.txt"), kDefaultFileMode, O_CREAT | O_WRONLY), 0);
  ASSERT_EQ(fixture_.RmDir(name), -1);
  EXPECT_EQ(errno, ENOTEMPTY);
}

TEST_F(DirectoryOpsTest, RmdirNonexistent) {
  ASSERT_EQ(fixture_.RmDir("no_such_dir"), -1);
  EXPECT_EQ(errno, ENOENT);
}

TEST_F(DirectoryOpsTest, RmdirOnFile) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  ASSERT_EQ(fixture_.RmDir(name), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, UnlinkDirectoryFails) {
  ASSERT_EQ(fixture_.MkDir("d", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.UnlinkFile("d"), -1);
  EXPECT_EQ(errno, EISDIR);
}

TEST_F(DirectoryOpsTest, RmdirRoot) {
  ASSERT_EQ(fixture_.RmDir("."), -1);
  EXPECT_TRUE(errno == EBUSY || errno == EINVAL);
}

// ────────────────────────────────────────────────────────────────
// readdir
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, ReaddirListsEntries) {
  ASSERT_EQ(fixture_.MkDir("d1", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir("d2", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile("f1.txt", kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile("f1.txt", ""), 0);
  ASSERT_EQ(fixture_.CreateFile("f2.txt", kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile("f2.txt", ""), 0);

  std::vector<std::string> entries;
  fixture_.ReadDir(".", &entries);
  EXPECT_EQ(entries.size(), 4u);
}

TEST_F(DirectoryOpsTest, ReaddirEmpty) {
  std::vector<std::string> entries;
  fixture_.ReadDir(".", &entries);
  EXPECT_TRUE(entries.empty());
}

TEST_F(DirectoryOpsTest, ReaddirOnFile) {
  const char *name = "f.txt";
  ASSERT_EQ(fixture_.CreateFile(name, kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "data"), 0);
  std::vector<std::string> entries;
  ASSERT_EQ(fixture_.ReadDir(name, &entries), -1);
  EXPECT_EQ(errno, ENOTDIR);
}

TEST_F(DirectoryOpsTest, ReaddirNonExistent) {
  std::vector<std::string> entries;
  ASSERT_EQ(fixture_.ReadDir("noent", &entries), -1);
  EXPECT_EQ(errno, ENOENT);
}

// ────────────────────────────────────────────────────────────────
// File operations on directory (error)
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, TruncateDir) {
  ASSERT_EQ(fixture_.MkDir("d", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.Truncate("d", 0), -1);
  EXPECT_EQ(errno, EISDIR);
}

TEST_F(DirectoryOpsTest, OpenDirectoryFails) {
  ASSERT_EQ(fixture_.MkDir("d", kDefaultDirMode), 0);
  int fd = fixture_.OpenFile("d", O_RDWR);
  ASSERT_EQ(fd, -1);
  EXPECT_EQ(errno, EISDIR);
}

// ────────────────────────────────────────────────────────────────
// Name length limits
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, MkdirNameAtLimit) {
  auto limits = fixture_.GetLimits();
  std::string name(limits.max_name_length, 'x');
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.RmDir(name), 0);
}

TEST_F(DirectoryOpsTest, MkdirNameTooLong) {
  auto limits = fixture_.GetLimits();
  std::string name(limits.max_name_length + 1, 'x');
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), -1);
  EXPECT_EQ(errno, ENAMETOOLONG);
}

// ────────────────────────────────────────────────────────────────
// Symlink / Hardlink on directories
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, SymlinkToDir) {
  ASSERT_EQ(fixture_.MkDir("d", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.Symlink("d", "link_to_d"), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Lstat("link_to_d", &st), 0);
  EXPECT_TRUE(S_ISLNK(st.st_mode));

  std::string target;
  ASSERT_EQ(fixture_.Readlink("link_to_d", &target), 0);
  EXPECT_EQ(target, "d");
}

TEST_F(DirectoryOpsTest, HardlinkToDirFails) {
  // POSIX: hard-linking directories is not allowed.
  ASSERT_EQ(fixture_.MkDir("d", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.HardLink("d", "d_link"), -1);
  EXPECT_EQ(errno, EPERM);
}

// ────────────────────────────────────────────────────────────────
// Test helpers
// ────────────────────────────────────────────────────────────────

struct Child {
  std::string name;
  std::string content;
  bool is_dir;
};

// ────────────────────────────────────────────────────────────────
// Directory rename — same parent
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RenameDir) {
  const std::string old_name = "olddir";
  const std::string new_name = "newdir";
  ASSERT_EQ(fixture_.MkDir(old_name, kDefaultDirMode), 0);

  const Child children[] = {
      {"sub", "", true},
      {"a.txt", "alpha", false},
      {"b.txt", "beta", false},
  };
  for (const auto& c : children) {
    auto path = old_name + "/" + c.name;
    if (c.is_dir) {
      ASSERT_EQ(fixture_.MkDir(path, kDefaultDirMode), 0);
    } else {
      ASSERT_EQ(fixture_.CreateFile(path, kDefaultFileMode, kDefaultCreateFlags), 0);
      ASSERT_EQ(fixture_.WriteFile(path, c.content), 0);
    }
  }

  struct stat before;
  ASSERT_EQ(fixture_.Stat(old_name, &before), 0);

  ASSERT_EQ(fixture_.Rename(old_name, new_name), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(old_name, &st), -1);
  EXPECT_EQ(errno, ENOENT);

  ASSERT_EQ(fixture_.Stat(new_name, &st), 0);
  EXPECT_EQ(st.st_ino, before.st_ino);
  EXPECT_TRUE(S_ISDIR(st.st_mode));
  EXPECT_EQ(st.st_nlink, before.st_nlink);
  EXPECT_TRUE(fixture_.UmaskEquals(new_name, kDefaultDirMode));

  for (const auto& c : children) {
    auto path = new_name + "/" + c.name;
    ASSERT_EQ(fixture_.Stat(path, &st), 0);
    if (c.is_dir) {
      EXPECT_TRUE(S_ISDIR(st.st_mode));
    } else {
      EXPECT_TRUE(S_ISREG(st.st_mode));
      EXPECT_TRUE(fixture_.FileEquals(path, c.content.size(),
                                      Fixture::Hash64(c.content)));
    }
  }
}

// ────────────────────────────────────────────────────────────────
// Directory rename — cross parent
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RenameDirCrossParent) {
  ASSERT_EQ(fixture_.MkDir("a", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir("a/b", kDefaultDirMode), 0);

  const Child children[] = {
      {"sub", "", true},
      {"f.txt", "data", false},
  };
  for (const auto& c : children) {
    auto path = "a/b/" + c.name;
    if (c.is_dir) {
      ASSERT_EQ(fixture_.MkDir(path, kDefaultDirMode), 0);
    } else {
      ASSERT_EQ(fixture_.CreateFile(path, kDefaultFileMode, kDefaultCreateFlags), 0);
      ASSERT_EQ(fixture_.WriteFile(path, c.content), 0);
    }
  }

  struct stat before;
  ASSERT_EQ(fixture_.Stat("a/b", &before), 0);

  ASSERT_EQ(fixture_.Rename("a/b", "b"), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat("a/b", &st), -1);
  EXPECT_EQ(errno, ENOENT);

  ASSERT_EQ(fixture_.Stat("b", &st), 0);
  EXPECT_EQ(st.st_ino, before.st_ino);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  for (const auto& c : children) {
    auto path = "b/" + c.name;
    ASSERT_EQ(fixture_.Stat(path, &st), 0);
    if (c.is_dir) {
      EXPECT_TRUE(S_ISDIR(st.st_mode));
    } else {
      EXPECT_TRUE(S_ISREG(st.st_mode));
      EXPECT_TRUE(fixture_.FileEquals(path, c.content.size(),
                                      Fixture::Hash64(c.content)));
    }
  }
}

// ────────────────────────────────────────────────────────────────
// Directory rename — over empty dir (replace)
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RenameDirOverEmptyDir) {
  const std::string src = "src";
  const std::string dst = "dst";
  ASSERT_EQ(fixture_.MkDir(src, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir(dst, kDefaultDirMode), 0);

  const Child children[] = {
      {"sub", "", true},
      {"a.txt", "alpha", false},
      {"b.txt", "beta", false},
  };
  for (const auto& c : children) {
    auto path = src + "/" + c.name;
    if (c.is_dir) {
      ASSERT_EQ(fixture_.MkDir(path, kDefaultDirMode), 0);
    } else {
      ASSERT_EQ(fixture_.CreateFile(path, kDefaultFileMode, kDefaultCreateFlags), 0);
      ASSERT_EQ(fixture_.WriteFile(path, c.content), 0);
    }
  }

  struct stat before;
  ASSERT_EQ(fixture_.Stat(src, &before), 0);

  ASSERT_EQ(fixture_.Rename(src, dst), 0);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(src, &st), -1);
  EXPECT_EQ(errno, ENOENT);

  ASSERT_EQ(fixture_.Stat(dst, &st), 0);
  EXPECT_EQ(st.st_ino, before.st_ino);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  for (const auto& c : children) {
    auto path = dst + "/" + c.name;
    ASSERT_EQ(fixture_.Stat(path, &st), 0);
    if (c.is_dir) {
      EXPECT_TRUE(S_ISDIR(st.st_mode));
    } else {
      EXPECT_TRUE(S_ISREG(st.st_mode));
      EXPECT_TRUE(fixture_.FileEquals(path, c.content.size(),
                                      Fixture::Hash64(c.content)));
    }
  }
}

// ────────────────────────────────────────────────────────────────
// Directory rename — over non-empty dir (error)
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RenameDirOverNonEmptyDir) {
  const std::string src = "src";
  const std::string dst = "dst";
  ASSERT_EQ(fixture_.MkDir(src, kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.MkDir(dst, kDefaultDirMode), 0);

  const Child src_children[] = {
      {"sub", "", true},
      {"a.txt", "alpha", false},
  };
  const Child dst_children[] = {
      {"child.txt", "x", false},
  };
  auto populate = [&](const std::string& parent, const Child* children,
                      size_t count) {
    for (size_t i = 0; i < count; ++i) {
      auto path = parent + "/" + children[i].name;
      if (children[i].is_dir) {
        ASSERT_EQ(fixture_.MkDir(path, kDefaultDirMode), 0);
      } else {
        ASSERT_EQ(fixture_.CreateFile(path, kDefaultFileMode, kDefaultCreateFlags), 0);
        ASSERT_EQ(fixture_.WriteFile(path, children[i].content), 0);
      }
    }
  };
  populate(src, src_children, std::size(src_children));
  populate(dst, dst_children, std::size(dst_children));

  struct stat src_before, dst_before;
  ASSERT_EQ(fixture_.Stat(src, &src_before), 0);
  ASSERT_EQ(fixture_.Stat(dst, &dst_before), 0);

  ASSERT_EQ(fixture_.Rename(src, dst), -1);
  EXPECT_EQ(errno, ENOTEMPTY);

  struct stat st;
  ASSERT_EQ(fixture_.Stat(src, &st), 0);
  EXPECT_EQ(st.st_ino, src_before.st_ino);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  ASSERT_EQ(fixture_.Stat(dst, &st), 0);
  EXPECT_EQ(st.st_ino, dst_before.st_ino);
  EXPECT_TRUE(S_ISDIR(st.st_mode));

  auto verify = [&](const std::string& parent, const Child* children,
                    size_t count) {
    for (size_t i = 0; i < count; ++i) {
      auto path = parent + "/" + children[i].name;
      ASSERT_EQ(fixture_.Stat(path, &st), 0);
      if (children[i].is_dir) {
        EXPECT_TRUE(S_ISDIR(st.st_mode));
      } else {
        EXPECT_TRUE(S_ISREG(st.st_mode));
        EXPECT_TRUE(fixture_.FileEquals(path, children[i].content.size(),
                                        Fixture::Hash64(children[i].content)));
      }
    }
  };
  verify(src, src_children, std::size(src_children));
  verify(dst, dst_children, std::size(dst_children));
}

// ────────────────────────────────────────────────────────────────
// Directory rename — to self (no-op)
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RenameDirToSelf) {
  const std::string name = "d";
  ASSERT_EQ(fixture_.MkDir(name, kDefaultDirMode), 0);

  const Child children[] = {
      {"sub", "", true},
      {"a.txt", "alpha", false},
  };
  for (const auto& c : children) {
    auto path = name + "/" + c.name;
    if (c.is_dir) {
      ASSERT_EQ(fixture_.MkDir(path, kDefaultDirMode), 0);
    } else {
      ASSERT_EQ(fixture_.CreateFile(path, kDefaultFileMode, kDefaultCreateFlags), 0);
      ASSERT_EQ(fixture_.WriteFile(path, c.content), 0);
    }
  }

  struct stat before;
  ASSERT_EQ(fixture_.Stat(name, &before), 0);

  ASSERT_EQ(fixture_.Rename(name, name), 0);

  struct stat after;
  ASSERT_EQ(fixture_.Stat(name, &after), 0);
  EXPECT_EQ(after.st_ino, before.st_ino);
  EXPECT_EQ(after.st_mode, before.st_mode);

  for (const auto& c : children) {
    auto path = name + "/" + c.name;
    struct stat st;
    ASSERT_EQ(fixture_.Stat(path, &st), 0);
    if (c.is_dir) {
      EXPECT_TRUE(S_ISDIR(st.st_mode));
    } else {
      EXPECT_TRUE(S_ISREG(st.st_mode));
      EXPECT_TRUE(fixture_.FileEquals(path, c.content.size(),
                                      Fixture::Hash64(c.content)));
    }
  }
}

// ────────────────────────────────────────────────────────────────
// Directory rename — over file (error)
// ────────────────────────────────────────────────────────────────

TEST_F(DirectoryOpsTest, RenameDirOverFile) {
  ASSERT_EQ(fixture_.MkDir("d", kDefaultDirMode), 0);
  ASSERT_EQ(fixture_.CreateFile("f.txt", kDefaultFileMode, kDefaultCreateFlags), 0);
  ASSERT_EQ(fixture_.Rename("d", "f.txt"), -1);
  EXPECT_EQ(errno, ENOTDIR);
}
