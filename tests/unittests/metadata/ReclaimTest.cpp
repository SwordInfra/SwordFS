// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for the frozen reclaim record — the durable, immutable object
// identities of an inode whose last directory entry is gone.
//
// The record is the only thing the delayed deletes are driven from, so its
// round-trip and its validation are a data-safety boundary: a record that
// parses with an identity the authoritative descriptor does not derive could
// delete another inode's live object. These tests pin the round-trip, the
// write-side argument validation, and every read-side rejection (malformed
// envelope, truncated identity, corrupt count, tampered key, trailing bytes).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "chunk/ChunkObjectKey.hpp"
#include "metadata/types/BufCodec.hpp"
#include "metadata/types/Chunk.hpp"
#include "metadata/types/Common.hpp"
#include "metadata/types/Reclaim.hpp"
#include "utils/Status.hpp"

namespace swordfs::metadata {
namespace {

using swordfs::utils::Status;

// One frozen chunk whose key is exactly the identity its descriptor derives,
// as every engine publishes it at freeze time.
ReclaimChunk FrozenChunk(InodeID ino, ChunkIndex index, ChunkRevision revision, uint64_t size = 64) {
  SwordFsChunk descriptor{
      .index = index, .start_offset = static_cast<uint64_t>(index) * size, .revision = revision, .size = size};
  return ReclaimChunk{descriptor, chunk::FormatChunkObjectKey(ino, index, revision)};
}

ReclaimWork MakeWork(InodeID ino = 42) {
  ReclaimWork work;
  work.ino = ino;
  work.chunks.push_back(FrozenChunk(ino, 0, 1));
  work.chunks.push_back(FrozenChunk(ino, 1, 2, 128));
  return work;
}

// Encode a record envelope by hand so a test can forge records the encoder
// would never produce: a zero inode, a tampered key, a corrupt count.
void EncodeRecord(uint64_t ino, const std::vector<std::pair<SwordFsChunk, std::string>> &chunks, std::string *out) {
  BufEncoder enc;
  enc.Header(RecordType::kReclaim);
  enc.U64(ino);
  enc.U32(static_cast<uint32_t>(chunks.size()));
  for (const auto &[descriptor, key] : chunks) {
    enc.U32(descriptor.index);
    enc.U64(descriptor.start_offset);
    enc.U64(descriptor.revision);
    enc.U64(descriptor.size);
    enc.String(key);
  }
  enc.Finish(out);
}

std::string SerializeOrDie(const ReclaimWork &work) {
  std::string blob;
  const auto status = work.SerializeTo(&blob);
  EXPECT_TRUE(status.ok()) << status.message();
  return blob;
}

// ────────────────────────────────────────────────────────────────
// Round-trip
// ────────────────────────────────────────────────────────────────

TEST(ReclaimWorkTest, RoundTripsEveryFrozenIdentity) {
  const ReclaimWork work = MakeWork();
  const std::string blob = SerializeOrDie(work);

  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(blob).ok());

  EXPECT_EQ(parsed, work);
  // The identity travels verbatim: the delete must never rebuild it from live
  // inode state, so the parsed key is exactly the frozen one.
  ASSERT_EQ(parsed.chunks.size(), 2U);
  EXPECT_EQ(parsed.chunks[0].key, chunk::FormatChunkObjectKey(42, 0, 1));
  EXPECT_EQ(parsed.chunks[1].key, chunk::FormatChunkObjectKey(42, 1, 2));
  EXPECT_EQ(parsed.chunks[1].descriptor.start_offset, 128U);
  EXPECT_EQ(parsed.chunks[1].descriptor.size, 128U);
}

TEST(ReclaimWorkTest, RoundTripsAnInodeWithoutChunks) {
  // An unlinked inode that never flushed a chunk has no object to delete; the
  // record still has to round-trip so the pending reclaim can be completed.
  ReclaimWork work;
  work.ino = 7;

  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(SerializeOrDie(work)).ok());

  EXPECT_EQ(parsed.ino, 7U);
  EXPECT_TRUE(parsed.chunks.empty());
  EXPECT_EQ(parsed, work);
}

TEST(ReclaimWorkTest, EqualityDetectsDifferentWorkAndChunkFields) {
  const ReclaimWork base = MakeWork();

  ReclaimWork different_ino = base;
  different_ino.ino++;
  EXPECT_NE(different_ino, base);

  ReclaimWork different_chunk_count = base;
  different_chunk_count.chunks.pop_back();
  EXPECT_NE(different_chunk_count, base);

  ReclaimWork different_descriptor = base;
  different_descriptor.chunks[0].descriptor.size++;
  EXPECT_NE(different_descriptor, base);

  ReclaimWork different_key = base;
  different_key.chunks[0].key.push_back('x');
  EXPECT_NE(different_key, base);
}

// ────────────────────────────────────────────────────────────────
// Write-side validation
// ────────────────────────────────────────────────────────────────

TEST(ReclaimWorkTest, RejectsNullOutput) {
  const ReclaimWork work = MakeWork();
  EXPECT_EQ(work.SerializeTo(nullptr).code(), Status::kInvalidArgument);
}

TEST(ReclaimWorkTest, RejectsMissingInode) {
  ReclaimWork work = MakeWork();
  work.ino = 0;

  std::string blob;
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);
}

TEST(ReclaimWorkTest, RejectsChunkWithoutARevision) {
  // Revision 0 is the invalid sentinel; a record carrying one would freeze an
  // identity that no published chunk can have.
  ReclaimWork work = MakeWork();
  work.chunks[1].descriptor.revision = kInvalidChunkRevision;

  std::string blob;
  EXPECT_EQ(work.SerializeTo(&blob).code(), Status::kInvalidArgument);
}

// ────────────────────────────────────────────────────────────────
// Read-side validation: malformed envelopes
// ────────────────────────────────────────────────────────────────

TEST(ReclaimWorkTest, RejectsEmptyBuffer) {
  ReclaimWork parsed = MakeWork();

  const auto status = parsed.ParseFrom(std::string_view());
  EXPECT_TRUE(status.IsMalformed()) << status.message();
}

TEST(ReclaimWorkTest, RejectsAnotherRecordType) {
  // A record of a different type must not be readable as a reclaim record:
  // the frozen identities of another record family are not identities at all.
  std::string blob;
  BufEncoder enc;
  enc.Header(RecordType::kInode);
  enc.U64(42);
  enc.U32(0);
  enc.Finish(&blob);

  ReclaimWork parsed;
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
}

TEST(ReclaimWorkTest, RejectsZeroInode) {
  std::string blob;
  EncodeRecord(0, {}, &blob);

  ReclaimWork parsed;
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
}

TEST(ReclaimWorkTest, RejectsTruncatedObjectIdentity) {
  const std::string blob = SerializeOrDie(MakeWork());

  ReclaimWork parsed;
  ASSERT_TRUE(parsed.ParseFrom(blob).ok()) << "the untruncated record must be readable";

  // Cut into the last key: its length prefix still reads, the bytes do not.
  std::string truncated = blob;
  truncated.resize(blob.size() - 4);
  EXPECT_TRUE(parsed.ParseFrom(truncated).IsMalformed());
}

TEST(ReclaimWorkTest, RejectsCorruptChunkCountWithoutSpinning) {
  // A corrupt count claims more chunks than the buffer holds. Each entry
  // consumes a bounded number of bytes, so the decoder must fail rather than
  // loop — the record is rejected instead of reporting a huge chunk list.
  std::string blob;
  BufEncoder enc;
  enc.Header(RecordType::kReclaim);
  enc.U64(42);
  enc.U32(0xFFFFFFFFu);  // a count no buffer could ever satisfy
  enc.U32(0);
  enc.U64(0);
  enc.U64(1);
  enc.U64(64);
  enc.String(chunk::FormatChunkObjectKey(42, 0, 1));
  enc.Finish(&blob);

  ReclaimWork parsed;
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
}

TEST(ReclaimWorkTest, RejectsTrailingData) {
  // Anything appended to a record is a schema violation: a reader that
  // tolerated it could be fed a second, unvalidated identity.
  const std::string blob = SerializeOrDie(MakeWork()) + "x";

  ReclaimWork parsed;
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
}

// ────────────────────────────────────────────────────────────────
// Read-side validation: corrupt / tampered identities
// ────────────────────────────────────────────────────────────────

TEST(ReclaimWorkTest, RejectsIdentityOfAnotherInode) {
  // The guard that matters most: a key that derives from a different inode
  // would delete that inode's live object, so the record must be rejected.
  std::string blob;
  EncodeRecord(
      42,
      {{SwordFsChunk{.index = 0, .start_offset = 0, .revision = 1, .size = 64}, chunk::FormatChunkObjectKey(43, 0, 1)}},
      &blob);

  ReclaimWork parsed;
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
}

TEST(ReclaimWorkTest, RejectsIdentityOfAnotherChunkSlot) {
  // Same inode, but the key names a different index/revision than the frozen
  // descriptor: the record is internally inconsistent.
  std::string blob;
  EncodeRecord(
      42,
      {{SwordFsChunk{.index = 3, .start_offset = 0, .revision = 1, .size = 64}, chunk::FormatChunkObjectKey(42, 4, 1)},
       {SwordFsChunk{.index = 5, .start_offset = 0, .revision = 2, .size = 64}, chunk::FormatChunkObjectKey(42, 5, 3)}},
      &blob);

  ReclaimWork parsed;
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
}

TEST(ReclaimWorkTest, RejectsIdentityWithInvalidRevision) {
  // A self-consistent but revision-less identity ("<ino>/0/0") passes the
  // derivation check and must still be rejected: revision 0 is never a
  // published chunk.
  std::string blob;
  EncodeRecord(42,
               {{SwordFsChunk{.index = 0, .start_offset = 0, .revision = kInvalidChunkRevision, .size = 64},
                 chunk::FormatChunkObjectKey(42, 0, kInvalidChunkRevision)}},
               &blob);

  ReclaimWork parsed;
  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
}

TEST(ReclaimWorkTest, FailedParseLeavesTheRecordUntouched) {
  // A rejected record must not be applied half-way: the caller keeps the
  // record it already had, so a failed read can never shrink a pending
  // reclaim's work to a prefix of its identities.
  ReclaimWork parsed = MakeWork();
  const ReclaimWork before = parsed;

  std::string blob;
  EncodeRecord(42,
               {{SwordFsChunk{.index = 0, .start_offset = 0, .revision = 1, .size = 64}, "42/0/1"},
                {SwordFsChunk{.index = 1, .start_offset = 64, .revision = 2, .size = 64}, "999/1/2"}},
               &blob);

  EXPECT_TRUE(parsed.ParseFrom(blob).IsMalformed());
  EXPECT_EQ(parsed, before);
}

}  // namespace
}  // namespace swordfs::metadata
