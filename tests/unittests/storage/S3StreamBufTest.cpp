// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <ostream>
#include <string>

#include "storage/s3/S3StreamBuf.hpp"

namespace swordfs::storage {
namespace {

TEST(S3StreamBufTest, WritesIntoCallerBufferAndReportsPosition) {
  char storage[8]{};
  PreallocatedOutputStreamBuf buffer(storage, sizeof(storage));
  std::ostream stream(&buffer);

  stream.write("abc", 3);
  ASSERT_TRUE(stream.good());
  EXPECT_EQ(stream.tellp(), 3);
  stream.flush();
  EXPECT_TRUE(stream.good());
  EXPECT_EQ(std::string(storage, 3), "abc");

  stream.seekp(1, std::ios_base::beg);
  EXPECT_TRUE(stream.fail());
}

TEST(S3StreamBufTest, StopsAtCapacityAndRejectsOverflow) {
  char storage[4]{};
  PreallocatedOutputStreamBuf buffer(storage, sizeof(storage));
  std::ostream stream(&buffer);

  stream.write("abcdef", 6);
  EXPECT_TRUE(stream.fail());
  EXPECT_EQ(std::string(storage, sizeof(storage)), "abcd");

  char single{};
  PreallocatedOutputStreamBuf single_buffer(&single, 1);
  std::ostream single_stream(&single_buffer);
  single_stream.put('x');
  ASSERT_TRUE(single_stream.good());
  single_stream.put('y');
  EXPECT_TRUE(single_stream.fail());
  EXPECT_EQ(single, 'x');
}

TEST(S3StreamBufTest, ResponseStreamUsesTheProvidedBuffer) {
  char storage[8]{};
  PreallocatedResponseStream stream(storage, sizeof(storage));

  stream.write("payload", 7);
  ASSERT_TRUE(stream.good());
  EXPECT_EQ(stream.tellp(), 7);
  EXPECT_EQ(std::string(storage, 7), "payload");
}

}  // namespace
}  // namespace swordfs::storage
