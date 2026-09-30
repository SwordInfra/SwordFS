// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <cerrno>

#include "metadata/redis/RedisReclaimCoordinator.hpp"
#include "metadata/types/Common.hpp"

namespace swordfs::metadata::redis::internal {
namespace {

SwordFsInode InodeWithNlink(uint32_t nlink) {
  SwordFsAttr attr(42, S_IFREG | 0644);
  attr.nlink = nlink;
  return SwordFsInode(42, attr, kRootInodeId);
}

TEST(RedisReclaimCoordinatorTest, RejectsInvalidInputs) {
  const ReclaimAttemptFn attempt = [] { return utils::Status::OK(); };
  const ReclaimInodeLookupFn lookup = [](SwordFsInode *) { return utils::Status::NotFound("absent"); };
  EXPECT_EQ(RunReclaimWithOutcomeReconciliation(0, attempt, lookup).ToErrno(), EINVAL);
  EXPECT_EQ(RunReclaimWithOutcomeReconciliation(1, {}, lookup).ToErrno(), EINVAL);
  EXPECT_EQ(RunReclaimWithOutcomeReconciliation(1, attempt, {}).ToErrno(), EINVAL);
}

TEST(RedisReclaimCoordinatorTest, ReturnsKnownOutcomeWithoutReconciliation) {
  int lookups = 0;
  const auto status = RunReclaimWithOutcomeReconciliation(
      3, [] { return utils::Status::Busy("known failure"); },
      [&](SwordFsInode *) {
        ++lookups;
        return utils::Status::NotFound("unused");
      });
  EXPECT_EQ(status.ToErrno(), EBUSY);
  EXPECT_EQ(lookups, 0);
}

TEST(RedisReclaimCoordinatorTest, AbsentInodeConvergesAmbiguousOutcomeToSuccess) {
  const auto status = RunReclaimWithOutcomeReconciliation(
      3, [] { return utils::Status::OutcomeUnknown("lost EXEC reply"); },
      [](SwordFsInode *) { return utils::Status::NotFound("reclaimed"); });
  EXPECT_TRUE(status.ok());
}

TEST(RedisReclaimCoordinatorTest, LinkedInodeMeansRevivalWon) {
  const auto status = RunReclaimWithOutcomeReconciliation(
      3, [] { return utils::Status::OutcomeUnknown("lost EXEC reply"); },
      [](SwordFsInode *inode) {
        *inode = InodeWithNlink(1);
        return utils::Status::OK();
      });
  EXPECT_TRUE(status.ok());
}

TEST(RedisReclaimCoordinatorTest, UnlinkedLiveInodeRetriesFromFreshSnapshot) {
  int attempts = 0;
  int lookups = 0;
  const auto status = RunReclaimWithOutcomeReconciliation(
      3,
      [&] {
        ++attempts;
        if (attempts == 1) {
          return utils::Status::OutcomeUnknown("lost EXEC reply");
        }
        return utils::Status::OK();
      },
      [&](SwordFsInode *inode) {
        ++lookups;
        *inode = InodeWithNlink(0);
        return utils::Status::OK();
      });
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(attempts, 2);
  EXPECT_EQ(lookups, 1);
}

TEST(RedisReclaimCoordinatorTest, ReconciliationLookupFailureIsObservable) {
  const auto status = RunReclaimWithOutcomeReconciliation(
      3, [] { return utils::Status::OutcomeUnknown("lost EXEC reply"); },
      [](SwordFsInode *) { return utils::Status::IOError("lookup failed"); });
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(status.message(), "lookup failed");
}

TEST(RedisReclaimCoordinatorTest, RetryBudgetPreservesAmbiguousOutcome) {
  int attempts = 0;
  const auto status = RunReclaimWithOutcomeReconciliation(
      2,
      [&] {
        ++attempts;
        return utils::Status::OutcomeUnknown("still ambiguous");
      },
      [](SwordFsInode *inode) {
        *inode = InodeWithNlink(0);
        return utils::Status::OK();
      });
  EXPECT_TRUE(status.IsOutcomeUnknown());
  EXPECT_EQ(attempts, 2);
}

}  // namespace
}  // namespace swordfs::metadata::redis::internal
