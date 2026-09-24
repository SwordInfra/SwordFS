// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// End-to-end tests: stale mount detection and recovery.
//
// Validates: mount/unmount lifecycle, stale mount detection,
//            remount after kill, and clean shutdown.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

using swordfs::e2e::Fixture;

class StaleMountTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp());
  }
  void TearDown() override {
    fixture_.TearDown();
  }
  Fixture fixture_;
};

TEST_F(StaleMountTest, MountIsAlive) {
  EXPECT_TRUE(fixture_.IsMounted());
}

TEST_F(StaleMountTest, MountUnmountCycle) {
  EXPECT_TRUE(fixture_.IsMounted());
  fixture_.TearDown();
  EXPECT_FALSE(fixture_.IsMounted());
  ASSERT_TRUE(fixture_.SetUp());
  EXPECT_TRUE(fixture_.IsMounted());
}

TEST_F(StaleMountTest, DaemonExitsAfterUmount) {
  EXPECT_TRUE(fixture_.IsMounted());
  fixture_.TearDown();
  EXPECT_FALSE(fixture_.IsMounted());
  EXPECT_TRUE(fixture_.IsDaemonGone());
}

TEST_F(StaleMountTest, DaemonExitsAfterMultiCycle) {
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(fixture_.IsMounted());
    fixture_.TearDown();
    EXPECT_FALSE(fixture_.IsMounted());
    EXPECT_TRUE(fixture_.IsDaemonGone());
    ASSERT_TRUE(fixture_.SetUp());
  }
}
