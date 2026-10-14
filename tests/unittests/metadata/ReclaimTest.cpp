// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chunk/cow/COWCleanup.hpp"
#include "chunk/internal/ChunkCleanupWorkEncoder.hpp"
#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Reclaim.hpp"

namespace swordfs::metadata {
namespace {

ReclaimWork MakeWork(InodeID ino = 42) {
  ReclaimWork work;
  EXPECT_TRUE(chunk::cow::FreezeCOWDetachedReclaim(ino, {{0, ChunkID(1)}, {1, ChunkID(2)}}, &work).ok());
  return work;
}

PendingDelete MakePendingDelete(InodeID ino = 42) {
  PendingDelete pending;
  EXPECT_TRUE(chunk::cow::FreezeCOWDetachedDelete(ino, {3, ChunkID(7)}, &pending).ok());
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

TEST(ReclaimWorkTest, RoundTripsOpaqueEnvelopeAndTypedChunkIDMappings) {
  const ReclaimWork original = MakeWork();
  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  EXPECT_EQ(parsed, original);
  EXPECT_EQ(parsed.ino, 42U);

  BufDecoder dec(parsed.payload);
  uint32_t kind = 0;
  uint64_t ino = 0;
  uint32_t count = 0;
  uint64_t index = 0;
  uint64_t id = 0;
  ASSERT_TRUE(dec.Header(RecordType::kCowTypedCleanup));
  ASSERT_TRUE(dec.U32(&kind));
  ASSERT_TRUE(dec.U64(&ino));
  ASSERT_TRUE(dec.U32(&count));
  EXPECT_EQ(kind, static_cast<uint32_t>(chunk::cow::COWCleanupKind::kDetachedReclaim));
  EXPECT_EQ(ino, 42U);
  EXPECT_EQ(count, 2U);
  ASSERT_TRUE(dec.U64(&index));
  ASSERT_TRUE(dec.U64(&id));
  EXPECT_EQ(index, 0U);
  EXPECT_EQ(id, 1U);
  ASSERT_TRUE(dec.U64(&index));
  ASSERT_TRUE(dec.U64(&id));
  EXPECT_EQ(index, 1U);
  EXPECT_EQ(id, 2U);
  EXPECT_TRUE(dec.Done());
}

TEST(ReclaimWorkTest, TypedChunkIndexPreserves64BitIdentity) {
  constexpr ChunkIndex kHighIndex = (uint64_t{1} << 32) + 9;
  ReclaimWork work;
  ASSERT_TRUE(chunk::cow::FreezeCOWDetachedReclaim(42, {{kHighIndex, ChunkID(17)}}, &work).ok());
  BufDecoder dec(work.payload);
  uint32_t kind = 0;
  uint32_t count = 0;
  uint64_t ino = 0;
  uint64_t index = 0;
  uint64_t id = 0;
  ASSERT_TRUE(dec.Header(RecordType::kCowTypedCleanup));
  dec.U32(&kind);
  dec.U64(&ino);
  dec.U32(&count);
  dec.U64(&index);
  dec.U64(&id);
  EXPECT_TRUE(dec.Done());
  EXPECT_EQ(index, kHighIndex);
  EXPECT_EQ(id, 17U);
}

TEST(ReclaimWorkTest, EmptyInodeStillFreezesAValidTypedPayload) {
  ReclaimWork work;
  ASSERT_TRUE(chunk::cow::FreezeCOWDetachedReclaim(7, {}, &work).ok());
  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(work)).ok());
  EXPECT_EQ(parsed, work);
  EXPECT_FALSE(parsed.payload.empty());
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

TEST(ReclaimWorkTest, TypedCleanupFreezeRejectsInvalidReferences) {
  ReclaimWork work;
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedReclaim(0, {}, &work).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedReclaim(42, {}, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedReclaim(42, {{0, ChunkID(0)}}, &work).ToErrno(), EINVAL);
}

TEST(PendingDeleteTest, RoundTripsOpaqueEnvelopeAndTypedChunkID) {
  const auto original = MakePendingDelete();
  PendingDelete parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  EXPECT_EQ(parsed, original);
  EXPECT_EQ(parsed.id, "cow:chunk:7");
  BufDecoder dec(parsed.payload);
  uint32_t kind = 0;
  uint64_t ino = 0;
  uint64_t index = 0;
  uint64_t id = 0;
  ASSERT_TRUE(dec.Header(RecordType::kCowTypedCleanup));
  dec.U32(&kind);
  dec.U64(&ino);
  dec.U64(&index);
  dec.U64(&id);
  EXPECT_TRUE(dec.Done());
  EXPECT_EQ(kind, static_cast<uint32_t>(chunk::cow::COWCleanupKind::kDetachedChunk));
  EXPECT_EQ(ino, 42U);
  EXPECT_EQ(index, 3U);
  EXPECT_EQ(id, 7U);
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

TEST(PendingDeleteTest, TypedCleanupFreezeRejectsInvalidIdentity) {
  PendingDelete pending;
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedDelete(0, {0, ChunkID(1)}, &pending).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedDelete(42, {0, ChunkID(0)}, &pending).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDetachedDelete(42, {0, ChunkID(1)}, nullptr).ToErrno(), EINVAL);
}

TEST(ChunkCleanupWorkEncoderTest, SelectsCOWAndEncodesOnlyDetachedTypedIdentities) {
  std::unique_ptr<chunk::internal::ChunkCleanupWorkEncoder> encoder;
  ASSERT_TRUE(chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kCow, &encoder).ok());
  ASSERT_NE(encoder, nullptr);

  PendingDelete pending;
  ASSERT_TRUE(encoder->EncodeDetachedDelete(42, {3, ChunkID(7)}, &pending).ok());
  EXPECT_EQ(pending, MakePendingDelete());

  ReclaimWork reclaim;
  ASSERT_TRUE(encoder->EncodeDetachedReclaim(42, {{0, ChunkID(1)}, {1, ChunkID(2)}}, &reclaim).ok());
  EXPECT_EQ(reclaim, MakeWork());
  EXPECT_EQ(encoder->EncodeDetachedDelete(42, {0, ChunkID(0)}, &pending).ToErrno(), EINVAL);
  EXPECT_EQ(encoder->EncodeDetachedReclaim(42, {{0, ChunkID(0)}}, &reclaim).ToErrno(), EINVAL);
}

TEST(ChunkCleanupWorkEncoderTest, InvalidConstructionFailsWithoutChangingOutput) {
  std::unique_ptr<chunk::internal::ChunkCleanupWorkEncoder> encoder;
  EXPECT_EQ(chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kCow, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::internal::CreateChunkCleanupWorkEncoder(ChunkType::kRedisCache, &encoder).ToErrno(), ENOSYS);
  EXPECT_EQ(encoder, nullptr);
}

}  // namespace
}  // namespace swordfs::metadata
