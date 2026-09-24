// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <string>

#include "utils/Logging.hpp"

namespace swordfs::utils {
namespace {

TEST(LoggingTest, RejectsInvalidLogLevel) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(InitLogging(LogConfig{.path = "/tmp/swordfs-unused.log", .level = "INVALID"}, true),
              ::testing::ExitedWithCode(1), "invalid log level");
}

TEST(LoggingTest, RejectsEmptyFilePath) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  EXPECT_EXIT(InitLogging(LogConfig{.path = "", .level = "INFO"}, false), ::testing::ExitedWithCode(1),
              "log file path is empty");
}

TEST(LoggingTest, RejectsUnopenableFilePath) {
  ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  const std::string path = "/swordfs-missing-parent-for-logging/log";
  EXPECT_EXIT(InitLogging(LogConfig{.path = path, .level = "INFO"}, false), ::testing::ExitedWithCode(1),
              "cannot open log file");
}

}  // namespace
}  // namespace swordfs::utils
