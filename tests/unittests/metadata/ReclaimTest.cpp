// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"
#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Reclaim.hpp"

namespace swordfs::metadata {
namespace {

std::unique_ptr<chunk::internal::ChunkCleanupWorkEncoder> MakeEncoder() {
  std::unique_ptr<chunk::internal::ChunkCleanupWorkEncoder> encoder;
  EXPECT_TRUE(chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kChunkSlice, &encoder).ok());
  return encoder;
}

ReclaimWork MakeWork(InodeID ino = 42) {
  ReclaimWork work;
  auto encoder = MakeEncoder();
  EXPECT_TRUE(encoder->EncodeDetachedReclaim(ino, {{0, ChunkID(1)}, {1, ChunkID(2)}}, &work).ok());
  return work;
}

PendingDelete MakePendingDelete(InodeID ino = 42) {
  PendingDelete pending;
  auto encoder = MakeEncoder();
  EXPECT_TRUE(encoder->EncodeDetachedDelete(ino, {3, ChunkID(7)}, &pending).ok());
  return pending;
}

template <typename Work>
std::string SerializeOrDie(const Work &work) {
  std::string encoded;
  const auto status = work.SerializeTo(&encoded);
  EXPECT_TRUE(status.ok()) << status.message();
  return encoded;
}

std::string EncodeReclaimEnvelope(InodeID ino, std::string_view payload) {
  BufEncoder enc;
  enc.Header(RecordType::kReclaim);
  enc.U64(ino);
  enc.String(payload);
  std::string encoded;
  enc.Finish(&encoded);
  return encoded;
}

std::string EncodePendingDeleteEnvelope(std::string_view id, std::string_view payload) {
  BufEncoder enc;
  enc.Header(RecordType::kPendingDelete);
  enc.String(id);
  enc.String(payload);
  std::string encoded;
  enc.Finish(&encoded);
  return encoded;
}

TEST(ReclaimWorkTest, RoundTripsOpaqueEnvelopeWithDeferredSliceMappings) {
  const ReclaimWork original = MakeWork();
  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  EXPECT_EQ(parsed, original);
  EXPECT_EQ(parsed.ino, 42U);
  EXPECT_EQ(parsed.payload, "slice:reclaim:42:0,1:1,2");
}

TEST(ReclaimWorkTest, DeferredSliceChunkIndexPreserves64BitIdentity) {
  constexpr ChunkIndex kHighIndex = (uint64_t{1} << 32) + 9;
  ReclaimWork work;
  ASSERT_TRUE(MakeEncoder()->EncodeDetachedReclaim(42, {{kHighIndex, ChunkID(17)}}, &work).ok());
  EXPECT_EQ(work.payload, "slice:reclaim:42:" + std::to_string(kHighIndex) + ",17");
}

TEST(ReclaimWorkTest, EmptyInodeStillFreezesAValidOpaquePayload) {
  ReclaimWork work;
  ASSERT_TRUE(MakeEncoder()->EncodeDetachedReclaim(7, {}, &work).ok());
  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(work)).ok());
  EXPECT_EQ(parsed, work);
  EXPECT_EQ(parsed.payload, "slice:reclaim:7");
}

TEST(ReclaimWorkTest, EqualityIncludesEnvelopeAndOpaquePayload) {
  const auto original = MakeWork();
  auto changed = original;
  ++changed.ino;
  EXPECT_NE(changed, original);
  changed = original;
  changed.payload.push_back('x');
  EXPECT_NE(changed, original);
}

TEST(ReclaimWorkTest, RejectsInvalidEnvelopeOnWriteAndRead) {
  ReclaimWork work = MakeWork();
  EXPECT_EQ(work.SerializeTo(nullptr).ToErrno(), EINVAL);
  std::string encoded;
  work.ino = 0;
  EXPECT_EQ(work.SerializeTo(&encoded).ToErrno(), EINVAL);
  work = MakeWork();
  work.payload.clear();
  EXPECT_EQ(work.SerializeTo(&encoded).ToErrno(), EINVAL);
  const ReclaimWork before = MakeWork();
  ReclaimWork parsed = before;
  EXPECT_EQ(parsed.ParseFrom("").ToErrno(), EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_EQ(parsed.ParseFrom(SerializeOrDie(before).substr(0, 8)).ToErrno(), EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_EQ(parsed.ParseFrom(SerializeOrDie(before) + "x").ToErrno(), EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_EQ(parsed.ParseFrom(EncodePendingDeleteEnvelope("other", "payload")).ToErrno(), EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_EQ(parsed.ParseFrom(EncodeReclaimEnvelope(0, "payload")).ToErrno(), EIO);
  EXPECT_EQ(parsed.ParseFrom(EncodeReclaimEnvelope(42, "")).ToErrno(), EIO);
}

TEST(PendingDeleteTest, RoundTripsOpaqueEnvelopeAndDeferredSliceIdentity) {
  const auto original = MakePendingDelete();
  PendingDelete parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  EXPECT_EQ(parsed, original);
  EXPECT_EQ(parsed.id, "slice:chunk:7");
  EXPECT_EQ(parsed.payload, "slice:detached:42:3:7");
}

TEST(PendingDeleteTest, RejectsInvalidEnvelopeOnWriteAndRead) {
  PendingDelete pending = MakePendingDelete();
  EXPECT_EQ(pending.SerializeTo(nullptr).ToErrno(), EINVAL);
  std::string encoded;
  pending.id.clear();
  EXPECT_EQ(pending.SerializeTo(&encoded).ToErrno(), EINVAL);
  pending = MakePendingDelete();
  pending.payload.clear();
  EXPECT_EQ(pending.SerializeTo(&encoded).ToErrno(), EINVAL);
  const PendingDelete before = MakePendingDelete();
  PendingDelete parsed = before;
  EXPECT_EQ(parsed.ParseFrom("").ToErrno(), EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_EQ(parsed.ParseFrom(SerializeOrDie(before) + "x").ToErrno(), EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_EQ(parsed.ParseFrom(EncodeReclaimEnvelope(42, "payload")).ToErrno(), EIO);
  EXPECT_EQ(parsed.ParseFrom(EncodePendingDeleteEnvelope("", "payload")).ToErrno(), EIO);
  EXPECT_EQ(parsed.ParseFrom(EncodePendingDeleteEnvelope("id", "")).ToErrno(), EIO);
}

TEST(ChunkCleanupWorkEncoderTest, RejectsRetiredAndUnknownMechanisms) {
  std::unique_ptr<chunk::internal::ChunkCleanupWorkEncoder> encoder;
  EXPECT_EQ(chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kCow, &encoder).ToErrno(), ENOSYS);
  EXPECT_EQ(chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kRedisCache, &encoder).ToErrno(), ENOSYS);
  EXPECT_EQ(encoder, nullptr);
  EXPECT_EQ(chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kChunkSlice, nullptr).ToErrno(), EINVAL);
}

}  // namespace
}  // namespace swordfs::metadata
