// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// The common reclaim envelope is opaque. Only the selected chunk mechanism may
// interpret or authorize deletion of the private payload.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chunk/cow/COWCleanup.hpp"
#include "chunk/cow/COWObjectKey.hpp"
#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {
namespace {

using swordfs::utils::Status;

constexpr uint64_t kChunkSize = 128;

SwordFsChunk Head(ChunkIndex index, ChunkRevision revision, uint64_t size = 64) {
  return SwordFsChunk{.index = index, .revision = revision, .size = size};
}

ReclaimWork MakeWork(InodeID ino = 42) {
  ReclaimWork work;
  EXPECT_TRUE(chunk::cow::FreezeCOWReclaim(ino, {Head(0, 1), Head(1, 2, 128)}, kChunkSize, &work).ok());
  return work;
}

PendingDelete MakePendingDelete(InodeID ino = 42) {
  PendingDelete work;
  EXPECT_TRUE(chunk::cow::FreezeCOWDelete(ino, Head(3, 7), kChunkSize, &work).ok());
  return work;
}

template <typename Work>
std::string SerializeOrDie(const Work &work) {
  std::string blob;
  const auto status = work.SerializeTo(&blob);
  EXPECT_TRUE(status.ok()) << status.message();
  return blob;
}

std::string EncodeReclaimEnvelope(InodeID ino, std::string_view payload,
                                  RecordType record_type = RecordType::kReclaim) {
  BufEncoder enc;
  enc.Header(record_type);
  enc.U64(ino);
  enc.String(payload);
  std::string blob;
  enc.Finish(&blob);
  return blob;
}

std::string EncodePendingDeleteEnvelope(std::string_view id, std::string_view payload,
                                        RecordType record_type = RecordType::kPendingDelete) {
  BufEncoder enc;
  enc.Header(record_type);
  enc.String(id);
  enc.String(payload);
  std::string blob;
  enc.Finish(&blob);
  return blob;
}

std::string EncodePrivateRefs(InodeID ino, const std::vector<chunk::cow::COWRef> &refs, bool trailing = false,
                              RecordType record_type = RecordType::kCowCleanup) {
  BufEncoder enc;
  enc.Header(record_type);
  enc.U64(ino);
  enc.U32(static_cast<uint32_t>(refs.size()));
  for (const auto &ref : refs) {
    enc.U64(ref.descriptor.index);
    enc.U64(ref.descriptor.revision);
    enc.U64(ref.descriptor.size);
    enc.String(ref.key);
  }
  if (trailing) {
    enc.U64(123);
  }
  std::string result;
  enc.Finish(&result);
  return result;
}

TEST(ReclaimWorkTest, RoundTripsOpaqueEnvelopeAndPrivateFrozenIdentities) {
  const ReclaimWork original = MakeWork();
  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  EXPECT_EQ(parsed, original);
  EXPECT_EQ(parsed.ino, 42U);

  std::vector<chunk::cow::COWRef> refs;
  ASSERT_TRUE(chunk::cow::DecodeCOWReclaim(parsed, kChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 2U);
  EXPECT_EQ(refs[0].ino, 42U);
  EXPECT_EQ(refs[0].key, chunk::cow::FormatCOWObjectKey(42, 0, 1));
  EXPECT_EQ(refs[1].key, chunk::cow::FormatCOWObjectKey(42, 1, 2));
  EXPECT_EQ(refs[1].descriptor.size, 128U);
}

TEST(ReclaimWorkTest, COWFrozenIdentitiesPreserveIndexAboveUint32Max) {
  constexpr uint64_t kHighIndex = (uint64_t{1} << 32) + 9;
  const SwordFsChunk high = Head(static_cast<ChunkIndex>(kHighIndex), 17);
  ASSERT_EQ(static_cast<uint64_t>(high.index), kHighIndex);

  ReclaimWork reclaim;
  ASSERT_TRUE(chunk::cow::FreezeCOWReclaim(42, {high}, kChunkSize, &reclaim).ok());
  std::vector<chunk::cow::COWRef> refs;
  ASSERT_TRUE(chunk::cow::DecodeCOWReclaim(reclaim, kChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(static_cast<uint64_t>(refs[0].descriptor.index), kHighIndex);
  EXPECT_EQ(refs[0].key, "42/" + std::to_string(kHighIndex) + "/17");

  PendingDelete pending;
  ASSERT_TRUE(chunk::cow::FreezeCOWDelete(42, high, kChunkSize, &pending).ok());
  chunk::cow::COWRef ref;
  ASSERT_TRUE(chunk::cow::DecodeCOWDelete(pending, kChunkSize, &ref).ok());
  EXPECT_EQ(static_cast<uint64_t>(ref.descriptor.index), kHighIndex);
  EXPECT_EQ(ref.key, "42/" + std::to_string(kHighIndex) + "/17");
}

TEST(ReclaimWorkTest, EmptyInodeStillHasDecodablePrivatePayload) {
  ReclaimWork original;
  ASSERT_TRUE(chunk::cow::FreezeCOWReclaim(7, {}, kChunkSize, &original).ok());
  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  std::vector<chunk::cow::COWRef> refs;
  ASSERT_TRUE(chunk::cow::DecodeCOWReclaim(parsed, kChunkSize, &refs).ok());
  EXPECT_TRUE(refs.empty());
}

TEST(ReclaimWorkTest, EqualityIncludesEnvelopeAndOpaquePayload) {
  const ReclaimWork original = MakeWork();
  auto changed = original;
  changed.ino++;
  EXPECT_NE(changed, original);
  changed = original;
  changed.payload.push_back('x');
  EXPECT_NE(changed, original);
}

TEST(ReclaimWorkTest, RejectsInvalidEnvelopeOnWriteAndRead) {
  ReclaimWork work = MakeWork();
  EXPECT_EQ(work.SerializeTo(nullptr).ToErrno(), EINVAL);
  std::string blob;
  work.ino = 0;
  EXPECT_EQ(work.SerializeTo(&blob).ToErrno(), EINVAL);
  work = MakeWork();
  work.payload.clear();
  EXPECT_EQ(work.SerializeTo(&blob).ToErrno(), EINVAL);

  ReclaimWork parsed = MakeWork();
  const auto before = parsed;
  EXPECT_TRUE(parsed.ParseFrom("").ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(SerializeOrDie(before).substr(0, 8)).ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(SerializeOrDie(before) + "x").ToErrno() == EIO);
  EXPECT_EQ(parsed, before);

  EXPECT_TRUE(parsed.ParseFrom(EncodePendingDeleteEnvelope("other-record", "payload")).ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(EncodeReclaimEnvelope(0, "payload")).ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(EncodeReclaimEnvelope(42, "")).ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
}

TEST(ReclaimWorkTest, PrivateDecoderRejectsWrongRecordTypeAndTamperedIdentities) {
  ReclaimWork work = MakeWork();
  std::vector<chunk::cow::COWRef> refs;
  work.payload = EncodePrivateRefs(42, {}, false, RecordType::kPendingDelete);
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);
  work = MakeWork();
  work.payload = EncodePrivateRefs(43, {{43, Head(0, 1), chunk::cow::FormatCOWObjectKey(43, 0, 1)}});
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);
  work.payload = EncodePrivateRefs(42, {{42, Head(0, 1), chunk::cow::FormatCOWObjectKey(43, 0, 1)}});
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);
  work.payload = EncodePrivateRefs(42, {{42, Head(0, 0), chunk::cow::FormatCOWObjectKey(42, 0, 0)}});
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);
  work.payload = EncodePrivateRefs(42, {{42, Head(0, 1), chunk::cow::FormatCOWObjectKey(42, 0, 1)}}, true);
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);
  work.payload.resize(work.payload.size() - 4);
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);
}

TEST(ReclaimWorkTest, COWCodecRejectsInvalidArgumentsAndTruncatedPayload) {
  ReclaimWork work;
  EXPECT_EQ(chunk::cow::FreezeCOWReclaim(0, {}, kChunkSize, &work).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWReclaim(42, {}, kChunkSize, nullptr).ToErrno(), EINVAL);

  auto invalid = Head(1, 2, kChunkSize + 1);
  EXPECT_EQ(chunk::cow::FreezeCOWReclaim(42, {invalid}, kChunkSize, &work).ToErrno(), EINVAL);

  work = MakeWork();
  EXPECT_EQ(chunk::cow::DecodeCOWReclaim(work, kChunkSize, nullptr).ToErrno(), EINVAL);
  work.payload.resize(5);
  std::vector<chunk::cow::COWRef> refs;
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);

  work = MakeWork();
  work.payload = EncodePrivateRefs(0, {});
  EXPECT_TRUE(chunk::cow::DecodeCOWReclaim(work, kChunkSize, &refs).ToErrno() == EIO);
}

TEST(PendingDeleteTest, RoundTripsOpaqueEnvelopeAndPrivateIdentity) {
  const PendingDelete original = MakePendingDelete();
  PendingDelete parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  EXPECT_EQ(parsed, original);
  EXPECT_EQ(parsed.id, "cow:" + chunk::cow::FormatCOWObjectKey(42, 3, 7));

  chunk::cow::COWRef ref;
  ASSERT_TRUE(chunk::cow::DecodeCOWDelete(parsed, kChunkSize, &ref).ok());
  EXPECT_EQ(ref.ino, 42U);
  EXPECT_EQ(ref.descriptor, Head(3, 7));
  EXPECT_EQ(ref.key, chunk::cow::FormatCOWObjectKey(42, 3, 7));
}

TEST(PendingDeleteTest, RejectsInvalidEnvelopeOnWriteAndRead) {
  PendingDelete work = MakePendingDelete();
  EXPECT_EQ(work.SerializeTo(nullptr).ToErrno(), EINVAL);
  std::string blob;
  work.id.clear();
  EXPECT_EQ(work.SerializeTo(&blob).ToErrno(), EINVAL);
  work = MakePendingDelete();
  work.payload.clear();
  EXPECT_EQ(work.SerializeTo(&blob).ToErrno(), EINVAL);

  PendingDelete parsed = MakePendingDelete();
  const auto before = parsed;
  EXPECT_TRUE(parsed.ParseFrom("broken").ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(SerializeOrDie(before) + "x").ToErrno() == EIO);
  EXPECT_EQ(parsed, before);

  EXPECT_TRUE(parsed.ParseFrom(EncodeReclaimEnvelope(42, "payload")).ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(EncodePendingDeleteEnvelope("", "payload")).ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(EncodePendingDeleteEnvelope("cow:42/3/7", "")).ToErrno() == EIO);
  EXPECT_EQ(parsed, before);
}

TEST(PendingDeleteTest, PrivateDecoderRejectsTamperingBeforeDeletion) {
  PendingDelete work = MakePendingDelete();
  chunk::cow::COWRef ref;
  work.payload = EncodePrivateRefs(42, {}, false, RecordType::kReclaim);
  EXPECT_TRUE(chunk::cow::DecodeCOWDelete(work, kChunkSize, &ref).ToErrno() == EIO);
  work = MakePendingDelete();
  work.id += "tampered";
  EXPECT_TRUE(chunk::cow::DecodeCOWDelete(work, kChunkSize, &ref).ToErrno() == EIO);
  work = MakePendingDelete();
  work.payload = EncodePrivateRefs(42, {{42, Head(3, 7), chunk::cow::FormatCOWObjectKey(43, 3, 7)}});
  EXPECT_TRUE(chunk::cow::DecodeCOWDelete(work, kChunkSize, &ref).ToErrno() == EIO);
  work = MakePendingDelete();
  work.payload = EncodePrivateRefs(42, {{42, Head(3, 7), chunk::cow::FormatCOWObjectKey(42, 3, 7)},
                                        {42, Head(4, 8), chunk::cow::FormatCOWObjectKey(42, 4, 8)}});
  EXPECT_TRUE(chunk::cow::DecodeCOWDelete(work, kChunkSize, &ref).ToErrno() == EIO);
}

TEST(PendingDeleteTest, COWCodecRejectsInvalidArgumentsAndMalformedInode) {
  PendingDelete work;
  EXPECT_EQ(chunk::cow::FreezeCOWDelete(0, Head(0, 1), kChunkSize, &work).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDelete(42, Head(0, 1), kChunkSize, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(chunk::cow::FreezeCOWDelete(42, Head(0, 0), kChunkSize, &work).ToErrno(), EINVAL);

  work = MakePendingDelete();
  EXPECT_EQ(chunk::cow::DecodeCOWDelete(work, kChunkSize, nullptr).ToErrno(), EINVAL);
  chunk::cow::COWRef ref;
  work.payload = EncodePrivateRefs(0, {});
  EXPECT_TRUE(chunk::cow::DecodeCOWDelete(work, kChunkSize, &ref).ToErrno() == EIO);
  work = MakePendingDelete();
  work.payload.resize(5);
  EXPECT_TRUE(chunk::cow::DecodeCOWDelete(work, kChunkSize, &ref).ToErrno() == EIO);
}

}  // namespace
}  // namespace swordfs::metadata
