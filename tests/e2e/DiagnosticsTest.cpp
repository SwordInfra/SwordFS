// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "tests/e2e/Fixture.hpp"

using swordfs::e2e::Fixture;

class E2EDiagnosticsTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(fixture_.SetUp());
  }

  void TearDown() override {
    fixture_.TearDown();
  }

  Fixture fixture_;
};

// CI invokes this case explicitly as a controlled failure. Keeping it disabled
// prevents the normal required E2E suite from becoming intentionally red.
TEST_F(E2EDiagnosticsTest, DISABLED_PreservesDaemonGenerationsOnFailure) {
  ASSERT_TRUE(fixture_.Remount());
  ADD_FAILURE() << "controlled failure used to verify E2E diagnostic retention";
}
