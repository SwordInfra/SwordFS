// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// The common reclaim envelope is opaque. Only the selected strategy may
// interpret or authorize deletion of the private payload.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "chunk/ChunkObjectKey.hpp"
#include "chunk/WholeObjectCleanup.hpp"
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
  return SwordFsChunk{
      .index = index, .start_offset = static_cast<uint64_t>(index) * kChunkSize, .revision = revision, .size = size};
}

ReclaimWork MakeWork(InodeID ino = 42) {
  ReclaimWork work;
  EXPECT_TRUE(chunk::FreezeWholeObjectReclaim(ino, {Head(0, 1), Head(1, 2, 128)}, kChunkSize, &work).ok());
  return work;
}

PendingDelete MakePendingDelete(InodeID ino = 42) {
  PendingDelete work;
  EXPECT_TRUE(chunk::FreezeWholeObjectDelete(ino, Head(3, 7), kChunkSize, &work).ok());
  return work;
}

template <typename Work>
std::string SerializeOrDie(const Work &work) {
  std::string blob;
  const auto status = work.SerializeTo(&blob);
  EXPECT_TRUE(status.ok()) << status.message();
  return blob;
}

std::string EncodePrivateRefs(InodeID ino, const std::vector<chunk::WholeObjectRef> &refs, bool trailing = false) {
  BufEncoder enc;
  enc.Header(RecordType::kWholeObjectCleanup);
  enc.U64(ino);
  enc.U32(static_cast<uint32_t>(refs.size()));
  for (const auto &ref : refs) {
    enc.U32(ref.descriptor.index);
    enc.U64(ref.descriptor.start_offset);
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
  EXPECT_EQ(parsed.index_format_version, 1U);

  std::vector<chunk::WholeObjectRef> refs;
  ASSERT_TRUE(chunk::DecodeWholeObjectReclaim(parsed, kChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 2U);
  EXPECT_EQ(refs[0].ino, 42U);
  EXPECT_EQ(refs[0].key, chunk::FormatChunkObjectKey(42, 0, 1));
  EXPECT_EQ(refs[1].key, chunk::FormatChunkObjectKey(42, 1, 2));
  EXPECT_EQ(refs[1].descriptor.size, 128U);
}

TEST(ReclaimWorkTest, EmptyInodeStillHasDecodablePrivatePayload) {
  ReclaimWork original;
  ASSERT_TRUE(chunk::FreezeWholeObjectReclaim(7, {}, kChunkSize, &original).ok());
  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  std::vector<chunk::WholeObjectRef> refs;
  ASSERT_TRUE(chunk::DecodeWholeObjectReclaim(parsed, kChunkSize, &refs).ok());
  EXPECT_TRUE(refs.empty());
}

TEST(ReclaimWorkTest, EqualityIncludesEnvelopeAndOpaquePayload) {
  const ReclaimWork original = MakeWork();
  auto changed = original;
  changed.ino++;
  EXPECT_NE(changed, original);
  changed = original;
  changed.index_format_version++;
  EXPECT_NE(changed, original);
  changed = original;
  changed.payload.push_back('x');
  EXPECT_NE(changed, original);
}

TEST(ReclaimWorkTest, RejectsInvalidEnvelopeOnWriteAndRead) {
  ReclaimWork work = MakeWork();
  EXPECT_EQ(work.SerializeTo(nullptr).code(), Status::kInvalidArgument);
  std::string blob;
  work.ino = 0;
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);
  work = MakeWork();
  work.index_format_version = 0;
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);
  work = MakeWork();
  work.payload.clear();
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);

  ReclaimWork parsed = MakeWork();
  const auto before = parsed;
  EXPECT_TRUE(parsed.ParseFrom("").IsMalformed());
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(SerializeOrDie(before).substr(0, 8)).IsMalformed());
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(SerializeOrDie(before) + "x").IsMalformed());
  EXPECT_EQ(parsed, before);

  BufEncoder enc;
  enc.Header(RecordType::kPendingDelete);
  enc.String("other-record");
  enc.U32(1);
  enc.String("payload");
  enc.Finish(&blob);
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
  EXPECT_EQ(parsed, before);
}

TEST(ReclaimWorkTest, PrivateDecoderRejectsTamperedIdentitiesAndUnknownVersion) {
  ReclaimWork work = MakeWork();
  std::vector<chunk::WholeObjectRef> refs;
  work.index_format_version = 99;
  EXPECT_EQ(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).code(), Status::kNotSupported);

  work = MakeWork();
  work.payload = EncodePrivateRefs(43, {{43, Head(0, 1), chunk::FormatChunkObjectKey(43, 0, 1)}});
  EXPECT_TRUE(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).IsMalformed());
  work.payload = EncodePrivateRefs(42, {{42, Head(0, 1), chunk::FormatChunkObjectKey(43, 0, 1)}});
  EXPECT_TRUE(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).IsMalformed());
  work.payload = EncodePrivateRefs(42, {{42, Head(0, 0), chunk::FormatChunkObjectKey(42, 0, 0)}});
  EXPECT_TRUE(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).IsMalformed());
  work.payload = EncodePrivateRefs(42, {{42, Head(0, 1), chunk::FormatChunkObjectKey(42, 0, 1)}}, true);
  EXPECT_TRUE(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).IsMalformed());
  work.payload.resize(work.payload.size() - 4);
  EXPECT_TRUE(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).IsMalformed());
}

TEST(ReclaimWorkTest, WholeObjectCodecRejectsInvalidArgumentsAndTruncatedPayload) {
  ReclaimWork work;
  EXPECT_EQ(chunk::FreezeWholeObjectReclaim(0, {}, kChunkSize, &work).code(), Status::kInvalidArgument);
  EXPECT_EQ(chunk::FreezeWholeObjectReclaim(42, {}, kChunkSize, nullptr).code(), Status::kInvalidArgument);

  auto invalid = Head(1, 2);
  invalid.start_offset = 0;
  EXPECT_EQ(chunk::FreezeWholeObjectReclaim(42, {invalid}, kChunkSize, &work).code(), Status::kInvalidArgument);

  work = MakeWork();
  EXPECT_EQ(chunk::DecodeWholeObjectReclaim(work, kChunkSize, nullptr).code(), Status::kInvalidArgument);
  work.payload.resize(5);
  std::vector<chunk::WholeObjectRef> refs;
  EXPECT_TRUE(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).IsMalformed());

  work = MakeWork();
  work.payload = EncodePrivateRefs(0, {});
  EXPECT_TRUE(chunk::DecodeWholeObjectReclaim(work, kChunkSize, &refs).IsMalformed());
}

TEST(PendingDeleteTest, RoundTripsOpaqueEnvelopeAndPrivateIdentity) {
  const PendingDelete original = MakePendingDelete();
  PendingDelete parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(original)).ok());
  EXPECT_EQ(parsed, original);
  EXPECT_EQ(parsed.id, "whole_object:" + chunk::FormatChunkObjectKey(42, 3, 7));

  chunk::WholeObjectRef ref;
  ASSERT_TRUE(chunk::DecodeWholeObjectDelete(parsed, kChunkSize, &ref).ok());
  EXPECT_EQ(ref.ino, 42U);
  EXPECT_EQ(ref.descriptor, Head(3, 7));
  EXPECT_EQ(ref.key, chunk::FormatChunkObjectKey(42, 3, 7));
}

TEST(PendingDeleteTest, RejectsInvalidEnvelopeOnWriteAndRead) {
  PendingDelete work = MakePendingDelete();
  EXPECT_EQ(work.SerializeTo(nullptr).code(), Status::kInvalidArgument);
  std::string blob;
  work.id.clear();
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);
  work = MakePendingDelete();
  work.index_format_version = 0;
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);
  work = MakePendingDelete();
  work.payload.clear();
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);

  PendingDelete parsed = MakePendingDelete();
  const auto before = parsed;
  EXPECT_TRUE(parsed.ParseFrom("broken").IsMalformed());
  EXPECT_EQ(parsed, before);
  EXPECT_TRUE(parsed.ParseFrom(SerializeOrDie(before) + "x").IsMalformed());
  EXPECT_EQ(parsed, before);
}

TEST(PendingDeleteTest, PrivateDecoderRejectsTamperingBeforeDeletion) {
  PendingDelete work = MakePendingDelete();
  chunk::WholeObjectRef ref;
  work.index_format_version = 99;
  EXPECT_EQ(chunk::DecodeWholeObjectDelete(work, kChunkSize, &ref).code(), Status::kNotSupported);

  work = MakePendingDelete();
  work.id += "tampered";
  EXPECT_TRUE(chunk::DecodeWholeObjectDelete(work, kChunkSize, &ref).IsMalformed());
  work = MakePendingDelete();
  work.payload = EncodePrivateRefs(42, {{42, Head(3, 7), chunk::FormatChunkObjectKey(43, 3, 7)}});
  EXPECT_TRUE(chunk::DecodeWholeObjectDelete(work, kChunkSize, &ref).IsMalformed());
  work = MakePendingDelete();
  work.payload = EncodePrivateRefs(42, {{42, Head(3, 7), chunk::FormatChunkObjectKey(42, 3, 7)},
                                        {42, Head(4, 8), chunk::FormatChunkObjectKey(42, 4, 8)}});
  EXPECT_TRUE(chunk::DecodeWholeObjectDelete(work, kChunkSize, &ref).IsMalformed());
}

TEST(PendingDeleteTest, WholeObjectCodecRejectsInvalidArgumentsAndMalformedInode) {
  PendingDelete work;
  EXPECT_EQ(chunk::FreezeWholeObjectDelete(0, Head(0, 1), kChunkSize, &work).code(), Status::kInvalidArgument);
  EXPECT_EQ(chunk::FreezeWholeObjectDelete(42, Head(0, 1), kChunkSize, nullptr).code(), Status::kInvalidArgument);
  EXPECT_EQ(chunk::FreezeWholeObjectDelete(42, Head(0, 0), kChunkSize, &work).code(), Status::kInvalidArgument);

  work = MakePendingDelete();
  EXPECT_EQ(chunk::DecodeWholeObjectDelete(work, kChunkSize, nullptr).code(), Status::kInvalidArgument);
  chunk::WholeObjectRef ref;
  work.payload = EncodePrivateRefs(0, {});
  EXPECT_TRUE(chunk::DecodeWholeObjectDelete(work, kChunkSize, &ref).IsMalformed());
  work = MakePendingDelete();
  work.payload.resize(5);
  EXPECT_TRUE(chunk::DecodeWholeObjectDelete(work, kChunkSize, &ref).IsMalformed());
}

}  // namespace
}  // namespace swordfs::metadata
