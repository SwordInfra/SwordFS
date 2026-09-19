// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <string>

#include "metadata/redis/RedisTestUtils.hpp"

namespace swordfs::test {
namespace {

TEST(RedisTestUtilsTest, GeneratesReadableUniqueNamespacesWithinProcess) {
  const auto first = UniqueRedisTestNamespace("redis-test", "same-suffix");
  const auto second = UniqueRedisTestNamespace("redis-test", "same-suffix");

  EXPECT_NE(first, second);
  EXPECT_EQ(first.rfind("redis-test-", 0), 0U);
  EXPECT_NE(first.find("-same-suffix"), std::string::npos);
  EXPECT_NE(second.find("-same-suffix"), std::string::npos);
}

}  // namespace
}  // namespace swordfs::test
