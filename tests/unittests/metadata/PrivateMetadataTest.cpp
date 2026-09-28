// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>

#include "metadata/IPrivateMetadata.hpp"

namespace swordfs::metadata {
namespace {

struct ExampleTxnCapability : IMechanismPrivateTxn {
  static constexpr ChunkOverwriteMechanism kMechanism = ChunkOverwriteMechanism::kChunkSlice;
};
struct OtherTxnCapability : IMechanismPrivateTxn {
  static constexpr ChunkOverwriteMechanism kMechanism = ChunkOverwriteMechanism::kChunkSlice;
};
struct WrongMechanismTxnCapability : IMechanismPrivateTxn {
  static constexpr ChunkOverwriteMechanism kMechanism = ChunkOverwriteMechanism::kRedisCache;
};

TEST(PrivateMetadataTest, CheckedSequenceRangeStartsAtOneAndFailsClosedAtPortableMaximum) {
  uint64_t current = 0;
  uint64_t value = 0;
  ASSERT_TRUE(internal::AllocatePrivateSequenceValue(&current, &value).ok());
  EXPECT_EQ(current, 1U);
  EXPECT_EQ(value, 1U);

  current = kMaxPrivateSequenceValue;
  EXPECT_EQ(internal::AllocatePrivateSequenceValue(&current, &value).ToErrno(), EIO);
  EXPECT_EQ(current, kMaxPrivateSequenceValue);
  EXPECT_EQ(internal::AllocatePrivateSequenceValue(nullptr, &value).ToErrno(), EINVAL);
  EXPECT_EQ(internal::AllocatePrivateSequenceValue(&current, nullptr).ToErrno(), EINVAL);
}

TEST(PrivateMetadataTest, TransactionContextBindsOneTypedCapabilityWithoutTypeErasureLeakage) {
  MechanismPrivateTxnContext context(ChunkOverwriteMechanism::kChunkSlice);
  ExampleTxnCapability capability;
  OtherTxnCapability other;
  WrongMechanismTxnCapability wrong_mechanism;

  EXPECT_EQ(context.mechanism(), ChunkOverwriteMechanism::kChunkSlice);
  EXPECT_EQ(context.Get<ExampleTxnCapability>(), nullptr);
  EXPECT_EQ(context.Bind<ExampleTxnCapability>(nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(context.Bind(&wrong_mechanism).ToErrno(), EINVAL);
  ASSERT_TRUE(context.Bind(&capability).ok());
  EXPECT_EQ(context.Get<ExampleTxnCapability>(), &capability);
  EXPECT_EQ(context.Get<OtherTxnCapability>(), nullptr);
  EXPECT_EQ(context.Bind(&other).ToErrno(), EEXIST);
}

}  // namespace
}  // namespace swordfs::metadata
