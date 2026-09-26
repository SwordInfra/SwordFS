// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <dirent.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <limits>
#include <thread>

#include "metadata/redis/RedisMetaImplTestBase.hpp"

namespace {

using swordfs::test::redis_meta::InodeID;
using swordfs::test::redis_meta::kRootInodeId;
using swordfs::test::redis_meta::kTestChunkSize;
using swordfs::test::redis_meta::MakePendingDelete;
using swordfs::test::redis_meta::PendingDeleteObjectKey;
using swordfs::test::redis_meta::ReclaimWork;
using swordfs::test::redis_meta::RedisMetaImpl;
using swordfs::test::redis_meta::RedisMetaImplTest;
using swordfs::test::redis_meta::SetAttrField;
using swordfs::test::redis_meta::Status;
using swordfs::test::redis_meta::SwordFsAttr;
using swordfs::test::redis_meta::SwordFsChunk;
using swordfs::test::redis_meta::SwordFsEntry;
using swordfs::test::redis_meta::SwordFsInode;
using swordfs::test::redis_meta::SwordFsVolume;

FIBER_TEST_F(RedisMetaImplTest, ReclaimKeepsLinkedInodesAndRemovesOrphans) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  // A linked inode is never reclaimable: preparation must change nothing.
  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  EXPECT_FALSE(work.has_value());
  ASSERT_TRUE(impl_->GetInode(file.ino, &file).ok());

  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());
  // Both steps are idempotent: repeating them is a no-op, not an error.
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  EXPECT_FALSE(work.has_value());
  EXPECT_TRUE(impl_->CompleteReclaim(file.ino).ok());
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteScanRejectsFieldValueIdentityMismatch) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  auto pending = MakePendingDelete(42, 0, 9);
  std::string encoded;
  ASSERT_TRUE(pending.SerializeTo(&encoded).ok());

  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "different/object/key", encoded); });

  bool has_more = false;
  EXPECT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1, [](const swordfs::metadata::PendingDelete &) { return Status::OK(); }, &has_more)
                  .ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteScanRejectsMalformedEnvelopeButLeavesPrivateValidationToStrategy) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "broken", "not-a-record"); });
  bool has_more = false;
  EXPECT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1, [](const swordfs::metadata::PendingDelete &) { return Status::OK(); }, &has_more)
                  .ToErrno() == EIO);

  swordfs::metadata::PendingDelete pending;
  SwordFsChunk invalid_extent{.index = 1, .revision = 9, .size = kTestChunkSize + 1};
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectDelete(42, invalid_extent, 0, &pending).ok());
  std::string encoded;
  ASSERT_TRUE(pending.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.del(key.PendingDeletes());
    redis.hset(key.PendingDeletes(), pending.id, encoded);
  });
  size_t visits = 0;
  EXPECT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1,
                      [&](const swordfs::metadata::PendingDelete &work) {
                        ++visits;
                        swordfs::chunk::WholeObjectRef ref;
                        EXPECT_TRUE(swordfs::chunk::DecodeWholeObjectDelete(work, kTestChunkSize, &ref).ToErrno() ==
                                    EIO);
                        return Status::OK();
                      },
                      &has_more)
                  .ok());
  EXPECT_EQ(visits, 1U);
}

FIBER_TEST_F(RedisMetaImplTest, UnlinkPublishesDurableOrphanCandidate) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, SwordFsChunk{.index = 0, .revision = 1, .size = 64}).ok());

  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  std::vector<InodeID> orphans;
  ASSERT_TRUE(impl_
                  ->VisitOrphanCandidates([&orphans](InodeID ino) {
                    orphans.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(orphans, std::vector<InodeID>{file.ino});

  // Durable, and nothing irreversible yet: the inode and its chunk metadata
  // stay until preparation freezes them.
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
    EXPECT_TRUE(redis.exists(key.Chunk(file.ino)));
  });
  SwordFsInode stored;
  ASSERT_TRUE(impl_->GetInode(file.ino, &stored).ok());
  EXPECT_EQ(stored.attr.nlink, 0U);
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimFreezesRecordAndRemovesLiveMetadata) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .revision = 1, .size = 64};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  std::optional<ReclaimWork> work;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &work).ok());
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*work, kTestChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, chunk);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 1));

  // The live metadata is gone and only the frozen record remains: the point
  // of no return has been crossed.
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
    EXPECT_FALSE(redis.exists(key.Inode(file.ino)));
    EXPECT_FALSE(redis.exists(key.Chunk(file.ino)));
    EXPECT_FALSE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
  });
  EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound());
  EXPECT_TRUE(impl_->Link(file.ino, kRootInodeId, "revived", nullptr).IsNotFound());

  // Replaying preparation returns the same frozen work without changing it:
  // that is exactly the crash-recovery path a later mount takes.
  std::optional<ReclaimWork> replay;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &replay).ok());
  EXPECT_EQ(replay, work);

  std::vector<InodeID> pending;
  ASSERT_TRUE(impl_
                  ->VisitPendingReclaims([&pending](const ReclaimWork &pending_work) {
                    pending.push_back(pending_work.ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(pending, std::vector<InodeID>{file.ino});

  // Completion drops the record, and only then: it is idempotent.
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_FALSE(redis.hexists(key.Reclaims(), std::to_string(file.ino))); });
  EXPECT_TRUE(impl_->CompleteReclaim(file.ino).ok());
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimIgnoresAdvisoryInodeCountState) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  const std::vector<std::string> counter_states = {"missing", "noncanonical", "wrong-type"};

  for (const auto &counter_state : counter_states) {
    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      redis.del(key.InodeCount());
      redis.set(key.InodeCount(), "1");
    });

    SwordFsInode file;
    ASSERT_TRUE(impl_->Create(kRootInodeId, "file-" + counter_state, 0644, &file).ok()) << counter_state;
    const SwordFsChunk chunk{.index = 0, .revision = 7, .size = 128};
    ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok()) << counter_state;
    ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file-" + counter_state).ok()) << counter_state;

    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      redis.del(key.InodeCount());
      if (counter_state == "noncanonical") {
        redis.set(key.InodeCount(), "02");
      } else if (counter_state == "wrong-type") {
        redis.hset(key.InodeCount(), "wrong", "type");
      }
    });

    std::optional<ReclaimWork> work;
    const auto status = impl_->PrepareReclaim(file.ino, &work);
    ASSERT_TRUE(status.ok()) << counter_state << ": " << status.message();
    ASSERT_TRUE(work.has_value()) << counter_state;
    EXPECT_EQ(work->ino, file.ino) << counter_state;
    EXPECT_TRUE(impl_->GetInode(file.ino, &file).IsNotFound()) << counter_state;

    RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
      EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino))) << counter_state;
    });

    swordfs::metadata::SwordFsStatFs stat;
    ASSERT_TRUE(impl_->StatFs(&stat).ok()) << counter_state;
    EXPECT_EQ(stat.files, impl_->GetLimits().max_free_inodes) << counter_state;
    EXPECT_EQ(stat.files_free, stat.files) << counter_state;
    ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok()) << counter_state;
  }
}

FIBER_TEST_F(RedisMetaImplTest, CrashLeftPendingReclaimIsRetriedFromPersistedState) {
  // Model a crash between "prepared" and "completed": the frozen record is in
  // Redis and the inode is gone. Recovery must hand back the same frozen
  // identities — nothing may be re-derived from live state (there is none).
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  SwordFsChunk chunk{.index = 0, .revision = 3, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, chunk).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  std::optional<ReclaimWork> prepared;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &prepared).ok());
  ASSERT_TRUE(prepared.has_value());
  // (crash here — the deletes never run)

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino))); });

  std::vector<InodeID> pending;
  ASSERT_TRUE(impl_
                  ->VisitPendingReclaims([&pending](const ReclaimWork &pending_work) {
                    pending.push_back(pending_work.ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(pending, std::vector<InodeID>{file.ino});

  std::optional<ReclaimWork> retried;
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino, &retried).ok());
  EXPECT_EQ(retried, prepared);
  ASSERT_TRUE(retried.has_value());
  std::vector<swordfs::chunk::WholeObjectRef> refs;
  ASSERT_TRUE(swordfs::chunk::DecodeWholeObjectReclaim(*retried, kTestChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].key, swordfs::chunk::FormatChunkObjectKey(file.ino, 0, 3));

  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
  ASSERT_TRUE(impl_->VisitPendingReclaims([](const ReclaimWork &) { return Status::OK(); }).ok());
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimRejectsFrozenRecordWithLiveInode) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "inconsistent-reclaim", 0644, &file).ok());
  const SwordFsChunk head{.index = 0, .revision = 7, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, head).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "inconsistent-reclaim").ok());

  ReclaimWork frozen;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(file.ino, {head}, kTestChunkSize, &frozen).ok());
  std::string encoded;
  ASSERT_TRUE(frozen.SerializeTo(&encoded).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    // This state is outside the current one-stage beta protocol. Reclaim must
    // fail closed rather than inventing compatibility recovery for it.
    redis.hset(key.Reclaims(), std::to_string(file.ino), encoded);
    EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
    EXPECT_TRUE(redis.exists(key.Chunk(file.ino)));
  });

  std::optional<ReclaimWork> replay;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &replay).ToErrno() == EBUSY);
  EXPECT_FALSE(replay.has_value());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
    EXPECT_TRUE(redis.exists(key.Chunk(file.ino)));
    EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
  });
}

FIBER_TEST_F(RedisMetaImplTest, LinkRevivesOrphanCandidateAndClearsMarker) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_TRUE(redis.hexists(key.Orphans(), std::to_string(file.ino))); });

  // The revival drops the marker in the same transaction that re-links the
  // inode, so reconciliation can never reclaim a linked inode.
  ASSERT_TRUE(impl_->Link(file.ino, kRootInodeId, "revived", nullptr).ok());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_FALSE(redis.hexists(key.Orphans(), std::to_string(file.ino))); });

  ReclaimWork stale;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(file.ino, {}, kTestChunkSize, &stale).ok());
  std::string encoded;
  ASSERT_TRUE(stale.SerializeTo(&encoded).ok());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), std::to_string(file.ino), encoded); });

  std::optional<ReclaimWork> work;
  // A frozen record alongside a live inode is outside the current one-stage
  // protocol. Never reinterpret or cancel it while the inode is live.
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ToErrno() == EBUSY);
  EXPECT_FALSE(work.has_value());
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { EXPECT_TRUE(redis.hexists(key.Reclaims(), std::to_string(file.ino))); });
  SwordFsInode stored;
  ASSERT_TRUE(impl_->GetInode(file.ino, &stored).ok());
  EXPECT_EQ(stored.attr.nlink, 1U);
}

FIBER_TEST_F(RedisMetaImplTest, MalformedPendingReclaimRecordIsRejected) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), "4242", "malformed"); });
  std::vector<InodeID> pending;
  EXPECT_TRUE(impl_
                  ->VisitPendingReclaims([&pending](const ReclaimWork &work) {
                    pending.push_back(work.ino);
                    return Status::OK();
                  })
                  .ToErrno() == EIO);

  // PrepareReclaim replays an existing frozen record before consulting live
  // inode state. A corrupt pending record must therefore fail closed here too
  // rather than being treated as a fresh reclaim.
  std::optional<ReclaimWork> replay;
  EXPECT_TRUE(impl_->PrepareReclaim(4242, &replay).ToErrno() == EIO);
  EXPECT_FALSE(replay.has_value());

  // A decodable record whose serialized inode disagrees with its hash field
  // is corrupt too: acting on it could delete another inode's objects.
  ReclaimWork record;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(7777, {}, kTestChunkSize, &record).ok());
  std::string serialized;
  ASSERT_TRUE(record.SerializeTo(&serialized).ok());
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), "4242", serialized); });
  EXPECT_TRUE(impl_->VisitPendingReclaims([](const ReclaimWork &) { return Status::OK(); }).ToErrno() == EIO);

  // A key that is not an inode id at all is not a candidate either.
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Reclaims(), "4242");
    redis.hset(key.Orphans(), "not-an-inode", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).ToErrno() == EIO);

  // Exercise the other field-parsing failures independently: a numeric
  // prefix with trailing junk, and the reserved zero inode id.
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Orphans(), "not-an-inode");
    redis.hset(key.Orphans(), "42x", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).ToErrno() == EIO);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    redis.hdel(key.Orphans(), "42x");
    redis.hset(key.Orphans(), "0", "1");
  });
  EXPECT_TRUE(impl_->VisitOrphanCandidates([](InodeID) { return Status::OK(); }).ToErrno() == EIO);
}

FIBER_TEST_F(RedisMetaImplTest, PrepareReclaimRejectsPendingRecordForAnotherInode) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  ReclaimWork wrong;
  ASSERT_TRUE(swordfs::chunk::FreezeWholeObjectReclaim(file.ino + 1, {}, kTestChunkSize, &wrong).ok());
  std::string serialized;
  ASSERT_TRUE(wrong.SerializeTo(&serialized).ok());

  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.Reclaims(), std::to_string(file.ino), serialized); });

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ToErrno() == EIO);
  EXPECT_FALSE(work.has_value());
}

FIBER_TEST_F(RedisMetaImplTest, MalformedChunkMetadataIsRejectedByPrepareReclaim) {
  // A corrupt chunk record must stop the freeze before it writes anything:
  // no pending record may exist and the live inode must survive.
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "file", 0644, &file).ok());
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) { redis.hset(key.Chunk(file.ino), "0", "malformed"); });
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "file").ok());

  ReclaimWork stale_output;
  stale_output.ino = 9999;
  std::optional<ReclaimWork> work = stale_output;
  EXPECT_TRUE(impl_->PrepareReclaim(file.ino, &work).ToErrno() == EIO);
  EXPECT_FALSE(work.has_value()) << "failed preparation must not leave stale caller-visible work";
  RunWithRawRedisFromFiber([&](sw::redis::Redis &redis) {
    EXPECT_FALSE(redis.hexists(key.Reclaims(), std::to_string(file.ino)));
    EXPECT_TRUE(redis.exists(key.Inode(file.ino)));
  });
  SwordFsInode stored;
  EXPECT_TRUE(impl_->GetInode(file.ino, &stored).ok());
}

FIBER_TEST_F(RedisMetaImplTest, VisitorArgumentsAreValidated) {
  // The visitors are how reconciliation reads the durable state: a null
  // visitor is a caller bug and must be refused before any Redis round-trip,
  // never read as "nothing to reclaim".
  EXPECT_EQ(impl_->VisitOrphanCandidates(swordfs::metadata::InodeVisitorFn{}).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->VisitPendingReclaims(swordfs::metadata::ReclaimVisitorFn{}).ToErrno(), EINVAL);
  bool has_more = false;
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(0, [](const auto &) { return Status::OK(); }, &has_more).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(1, swordfs::metadata::PendingDeleteVisitorFn{}, &has_more).ToErrno(),
            EINVAL);
  EXPECT_EQ(impl_->VisitPendingDeletesBatch(1, [](const auto &) { return Status::OK(); }, nullptr).ToErrno(), EINVAL);
  EXPECT_EQ(impl_->PrepareReclaim(kRootInodeId, nullptr).ToErrno(), EINVAL);

  std::optional<ReclaimWork> work;
  EXPECT_TRUE(impl_->PrepareReclaim(kRootInodeId, &work).ok());
  EXPECT_FALSE(work.has_value());
}

FIBER_TEST_F(RedisMetaImplTest, ReclaimVisitorsReturnSnapshotsAndPropagateAbort) {
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "first", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "second", 0644, &second).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "first").ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "second").ok());

  std::vector<InodeID> visited;
  ASSERT_TRUE(impl_
                  ->VisitOrphanCandidates([&](InodeID ino) {
                    visited.push_back(ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(visited, (std::vector<InodeID>{first.ino, second.ino}));

  visited.clear();
  const auto orphan_abort = impl_->VisitOrphanCandidates([&](InodeID ino) {
    visited.push_back(ino);
    return Status::Busy("stop orphan scan");
  });
  EXPECT_EQ(orphan_abort.ToErrno(), EBUSY);
  EXPECT_EQ(visited, (std::vector<InodeID>{first.ino}));

  std::optional<ReclaimWork> frozen;
  ASSERT_TRUE(impl_->PrepareReclaim(second.ino, &frozen).ok());
  ASSERT_TRUE(frozen.has_value());

  visited.clear();
  ASSERT_TRUE(impl_
                  ->VisitPendingReclaims([&](const ReclaimWork &work) {
                    visited.push_back(work.ino);
                    return Status::OK();
                  })
                  .ok());
  EXPECT_EQ(visited, (std::vector<InodeID>{second.ino}));

  visited.clear();
  const auto pending_abort = impl_->VisitPendingReclaims([&](const ReclaimWork &work) {
    visited.push_back(work.ino);
    return Status::IOError("stop pending scan");
  });
  EXPECT_EQ(pending_abort.ToErrno(), EIO);
  EXPECT_EQ(visited, (std::vector<InodeID>{second.ino}));
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchBoundsVisitsAndContinuesWhileQueueMutates) {
  constexpr size_t kRecordCount = 7;
  std::vector<std::string> expected;
  for (size_t i = 0; i < kRecordCount; ++i) {
    auto pending = MakePendingDelete(42, static_cast<swordfs::metadata::ChunkIndex>(i), i + 1);
    expected.push_back(PendingDeleteObjectKey(pending));
    SeedPendingDelete(pending);
  }
  std::sort(expected.begin(), expected.end());

  std::vector<std::string> visited;
  bool has_more = true;
  size_t calls = 0;
  while (has_more && calls++ < 32) {
    size_t visits_this_call = 0;
    auto status = impl_->VisitPendingDeletesBatch(
        2,
        [&](const swordfs::metadata::PendingDelete &work) {
          ++visits_this_call;
          visited.push_back(PendingDeleteObjectKey(work));
          return impl_->CompletePendingDelete(work.id);
        },
        &has_more);
    ASSERT_TRUE(status.ok()) << status.message();
    EXPECT_LE(visits_this_call, 2U);
  }
  EXPECT_LT(calls, 32U);
  EXPECT_FALSE(has_more);
  std::sort(visited.begin(), visited.end());
  visited.erase(std::unique(visited.begin(), visited.end()), visited.end());
  EXPECT_EQ(visited, expected);
  EXPECT_TRUE(PendingDeleteKeys().empty());
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchUncompletedIntentDoesNotStarveLaterRecords) {
  constexpr size_t kRecordCount = 7;
  auto live = MakePendingDelete(42, 0, 1);
  SeedPendingDelete(live);
  for (size_t i = 1; i < kRecordCount; ++i) {
    SeedPendingDelete(MakePendingDelete(42, static_cast<swordfs::metadata::ChunkIndex>(i), i + 1));
  }

  bool has_more = true;
  size_t calls = 0;
  while (has_more && calls++ < 64) {
    auto status = impl_->VisitPendingDeletesBatch(
        1,
        [&](const swordfs::metadata::PendingDelete &work) {
          if (PendingDeleteObjectKey(work) == PendingDeleteObjectKey(live)) {
            return Status::OK();
          }
          return impl_->CompletePendingDelete(work.id);
        },
        &has_more);
    ASSERT_TRUE(status.ok()) << status.message();
  }
  EXPECT_LT(calls, 64U);
  EXPECT_FALSE(has_more);
  EXPECT_EQ(PendingDeleteKeys(), std::vector<std::string>{PendingDeleteObjectKey(live)});
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchVisitorAbortRetriesCurrentBufferedRecord) {
  auto pending = MakePendingDelete(42, 0, 1);
  SeedPendingDelete(pending);

  bool has_more = false;
  std::string first_key;
  auto status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &work) {
        first_key = PendingDeleteObjectKey(work);
        return Status::Busy("stop bounded pending delete scan");
      },
      &has_more);
  EXPECT_TRUE(status.ToErrno() == EBUSY) << status.message();
  EXPECT_EQ(first_key, PendingDeleteObjectKey(pending));

  std::string retried_key;
  status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &work) {
        retried_key = PendingDeleteObjectKey(work);
        return Status::OK();
      },
      &has_more);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(retried_key, first_key);
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchMalformedRecordFailsBeforeVisitor) {
  const swordfs::metadata::redis::RedisKey key(config_.db, volume_name_);
  RunWithRawRedisFromFiber(
      [&](sw::redis::Redis &redis) { redis.hset(key.PendingDeletes(), "broken", "not-a-pending-delete"); });

  bool has_more = false;
  size_t visits = 0;
  const auto status = impl_->VisitPendingDeletesBatch(
      1,
      [&](const swordfs::metadata::PendingDelete &) {
        ++visits;
        return Status::OK();
      },
      &has_more);
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
  EXPECT_EQ(visits, 0U);
}

FIBER_TEST_F(RedisMetaImplTest, PendingDeleteBatchRestartRediscoversDurableRemainder) {
  auto first = MakePendingDelete(42, 0, 1);
  auto second = MakePendingDelete(42, 1, 2);
  SeedPendingDelete(first);
  SeedPendingDelete(second);

  bool has_more = false;
  size_t visits = 0;
  ASSERT_TRUE(impl_
                  ->VisitPendingDeletesBatch(
                      1,
                      [&](const swordfs::metadata::PendingDelete &) {
                        ++visits;
                        return Status::OK();
                      },
                      &has_more)
                  .ok());
  EXPECT_EQ(visits, 1U);
  EXPECT_TRUE(has_more);

  std::unique_ptr<RedisMetaImpl> peer;
  swordfs::test::RunInTestThreadFromFiber([&] {
    peer = std::make_unique<RedisMetaImpl>(config_, volume_name_);
    ASSERT_TRUE(peer->Initialize().ok());
    SwordFsVolume volume;
    volume.name = volume_name_;
    ASSERT_TRUE(peer->LoadVolume(&volume).ok());
  });

  std::vector<std::string> rediscovered;
  has_more = true;
  size_t calls = 0;
  while (has_more && calls++ < 16) {
    auto status = peer->VisitPendingDeletesBatch(
        1,
        [&](const swordfs::metadata::PendingDelete &work) {
          rediscovered.push_back(PendingDeleteObjectKey(work));
          return peer->CompletePendingDelete(work.id);
        },
        &has_more);
    ASSERT_TRUE(status.ok()) << status.message();
  }
  EXPECT_LT(calls, 16U);
  std::sort(rediscovered.begin(), rediscovered.end());
  rediscovered.erase(std::unique(rediscovered.begin(), rediscovered.end()), rediscovered.end());
  EXPECT_EQ(rediscovered.size(), 2U);
  EXPECT_TRUE(PendingDeleteKeys(peer.get()).empty());
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}
}  // namespace
