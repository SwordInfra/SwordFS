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

namespace {

// Count swordfs daemon processes (excludes the test binary itself).
int CountSwordfsDaemons() {
  int count = 0;
  pid_t my_pid = getpid();
  std::ifstream proc("/proc/self/status");
  pid_t my_ppid = 0;
  if (proc.is_open()) {
    std::string line;
    while (std::getline(proc, line)) {
      if (line.rfind("PPid:", 0) == 0) {
        my_ppid = std::stol(line.substr(5));
        break;
      }
    }
  }
  // Scan /proc for swordfs processes that are not our test binary.
  for (int pid = 2; pid < 32768; ++pid) {
    if (pid == my_pid || pid == my_ppid) {
      continue;
    }
    std::ifstream cmdline("/proc/" + std::to_string(pid) + "/cmdline");
    if (!cmdline.is_open()) {
      continue;
    }
    std::string buf((std::istreambuf_iterator<char>(cmdline)), std::istreambuf_iterator<char>());
    // cmdline uses '\0' as separator; check if "swordfs" appears.
    if (buf.find("swordfs") != std::string::npos) {
      ++count;
    }
  }
  return count;
}

}  // namespace

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
  EXPECT_EQ(CountSwordfsDaemons(), 0);
}

TEST_F(StaleMountTest, DaemonExitsAfterMultiCycle) {
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(fixture_.IsMounted());
    fixture_.TearDown();
    EXPECT_FALSE(fixture_.IsMounted());
    EXPECT_EQ(CountSwordfsDaemons(), 0);
    ASSERT_TRUE(fixture_.SetUp());
  }
}
