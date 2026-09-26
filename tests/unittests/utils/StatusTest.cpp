// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

#include "utils/Status.hpp"

namespace swordfs::utils {
namespace {

TEST(StatusTest, BackendTerminalStatusesRemainMachineReadableAndMapToEio) {
  const auto unavailable = Status::Unavailable("backend retry budget exhausted");
  EXPECT_TRUE(unavailable.IsUnavailable());
  EXPECT_FALSE(unavailable.IsOutcomeUnknown());
  EXPECT_EQ(unavailable.ToErrno(), EIO);
  EXPECT_EQ(unavailable.message(), "backend retry budget exhausted");

  const auto outcome_unknown = Status::OutcomeUnknown("remote mutation acknowledgement lost");
  EXPECT_TRUE(outcome_unknown.IsOutcomeUnknown());
  EXPECT_FALSE(outcome_unknown.IsUnavailable());
  EXPECT_EQ(outcome_unknown.ToErrno(), EIO);
  EXPECT_EQ(outcome_unknown.message(), "remote mutation acknowledgement lost");
}

}  // namespace
}  // namespace swordfs::utils
