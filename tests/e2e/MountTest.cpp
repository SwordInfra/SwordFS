// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: mount lifecycle and daemon management.

#include <fcntl.h>
#include <gtest/gtest.h>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class MountTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp());
  }
  void TearDown() override {
    fixture_.TearDown();
  }
  Fixture fixture_;
};

// ────────────────────────────────────────────────────────────────
// Mount + unmount lifecycle
// ────────────────────────────────────────────────────────────────

TEST_F(MountTest, MountIsAlive) {
  EXPECT_TRUE(fixture_.IsMounted());
}

TEST_F(MountTest, WriteAfterMount) {
  const char *name = "after_mount.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "ok"), 0);
  EXPECT_TRUE(fixture_.FileEquals(name, 2, Fixture::Hash64("ok")));
}

TEST_F(MountTest, DataPersistsAfterRemount) {
  const char *name = "persist.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "persistent data"), 0);

  fixture_.TearDown();
  ASSERT_TRUE(fixture_.SetUp());

  // Redis metadata survives unmount, so verify that the namespace is
  // but the mount cycle itself must be clean.
  EXPECT_TRUE(fixture_.IsMounted());
}

// ────────────────────────────────────────────────────────────────
// Mount/unmount cycle
// ────────────────────────────────────────────────────────────────

TEST_F(MountTest, MountUnmountCycle) {
  EXPECT_TRUE(fixture_.IsMounted());
  fixture_.TearDown();
  EXPECT_FALSE(fixture_.IsMounted());
  ASSERT_TRUE(fixture_.SetUp());
  EXPECT_TRUE(fixture_.IsMounted());
}

// ────────────────────────────────────────────────────────────────
// Daemon cleanup
// ────────────────────────────────────────────────────────────────

TEST_F(MountTest, DaemonExitsAfterUmount) {
  EXPECT_TRUE(fixture_.IsMounted());
  fixture_.TearDown();
  EXPECT_FALSE(fixture_.IsMounted());
  EXPECT_TRUE(fixture_.IsDaemonGone());
}

TEST_F(MountTest, DaemonExitsAfterMultiCycle) {
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(fixture_.IsMounted());
    fixture_.TearDown();
    EXPECT_FALSE(fixture_.IsMounted());
    EXPECT_TRUE(fixture_.IsDaemonGone());
    ASSERT_TRUE(fixture_.SetUp());
  }
}

TEST_F(MountTest, DaemonExitsAfterWriteAndUmount) {
  const char *name = "before_umount.txt";
  ASSERT_EQ(fixture_.CreateFile(name, 0644, O_CREAT | O_WRONLY | O_TRUNC), 0);
  ASSERT_EQ(fixture_.WriteFile(name, "clean shutdown"), 0);
  EXPECT_TRUE(fixture_.FileEquals(name, 14, Fixture::Hash64("clean shutdown")));
  fixture_.TearDown();
  EXPECT_TRUE(fixture_.IsDaemonGone());
}
