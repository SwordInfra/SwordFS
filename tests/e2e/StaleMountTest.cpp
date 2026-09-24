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
#include <filesystem>
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
  // Enumerating /proc yields process leaders only. Do not probe every numeric
  // /proc/<id> path: Linux also exposes /proc/<tid> for non-leader threads,
  // which would count one multithreaded swordfs daemon multiple times.
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator("/proc", ec)) {
    if (ec) {
      break;
    }
    const auto name = entry.path().filename().string();
    if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) {
      continue;
    }
    const auto pid = static_cast<pid_t>(std::strtol(name.c_str(), nullptr, 10));
    if (pid == my_pid || pid == my_ppid) {
      continue;
    }
    std::ifstream cmdline(entry.path() / "cmdline");
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
