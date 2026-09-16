// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "tests/e2e/Utils.hpp"

namespace swordfs::e2e {
namespace {

TEST(E2EUtilsTest, DeterministicPayloadHasStablePrefix) {
  constexpr std::array<uint8_t, 16> kExpected = {
      0x0d, 0x6b, 0xb0, 0x3b, 0x9f, 0x5d, 0xa2, 0xc1, 0x8f, 0x7c, 0xf2, 0x4a, 0x6b, 0x8f, 0xb4, 0x1b,
  };

  const std::string payload = MakeDeterministicPayload(kExpected.size());
  ASSERT_EQ(payload.size(), kExpected.size());
  for (size_t i = 0; i < kExpected.size(); ++i) {
    EXPECT_EQ(static_cast<uint8_t>(payload[i]), kExpected[i]) << "byte " << i;
  }
}

}  // namespace
}  // namespace swordfs::e2e
