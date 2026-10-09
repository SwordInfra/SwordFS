// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <thread>

#include "metadata/contracts/IMetaEngineContract.hpp"
#include "metadata/redis/RedisMetaImplTestBase.hpp"

namespace {

using swordfs::metadata::IMetaEngine;
using swordfs::test::redis_meta::kRootInodeId;
using swordfs::test::redis_meta::RedisMetaImpl;
using swordfs::test::redis_meta::RedisMetaImplTest;
using swordfs::test::redis_meta::Status;
using swordfs::test::redis_meta::SwordFsContext;
using swordfs::test::redis_meta::SwordFsInode;
using swordfs::test::redis_meta::SwordFsVolume;

std::unique_ptr<RedisMetaImpl> NewPeer(const swordfs::metadata::RedisMetaConfig &config,
                                       const std::string &volume_name) {
  return swordfs::test::RunInTestThreadFromFiber([&] {
    auto peer = std::make_unique<RedisMetaImpl>(config, volume_name);
    auto status = peer->Initialize();
    EXPECT_TRUE(status.ok()) << status.message();
    if (!status.ok()) {
      return std::unique_ptr<RedisMetaImpl>{};
    }
    SwordFsVolume volume;
    volume.name = volume_name;
    status = peer->LoadVolume(&volume);
    EXPECT_TRUE(status.ok()) << status.message();
    return status.ok() ? std::move(peer) : std::unique_ptr<RedisMetaImpl>{};
  });
}

FIBER_TEST_F(RedisMetaImplTest, ContractNamespaceMissingParentsDoNotPartiallyMutate) {
  swordfs::test::meta_contract::NamespaceRejectsMissingAndPreservesIdentity(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractDirectoryIteratorsRemainIndependent) {
  swordfs::test::meta_contract::DirectoryIterationIsAnIndependentOpaqueView(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractOrphanRevivabilityStopsAtReclaim) {
  swordfs::test::meta_contract::OrphanRevivalAndReclaimAreMutuallyExclusive(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractXAttrModesAndBinaryValues) {
  swordfs::test::meta_contract::XAttrsValidateCreateReplaceAndMissing(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractRenameCannotCreateDirectoryCycle) {
  swordfs::test::meta_contract::RenameRejectsAncestryCycles(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractCrossParentSameInodeExchangeIsNoOp) {
  swordfs::test::meta_contract::RenameSameInodeAcrossParentsIsNoOp(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractStickyDirectoryAllowsAuthorizedOwners) {
  swordfs::test::meta_contract::StickyOwnerExceptionsPreserveNamespace(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractInodeFlagIdempotencePreservesCtime) {
  swordfs::test::meta_contract::IdempotentInodePolicyDoesNotChangeCtime(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractMalformedAclFailsWithoutMutation) {
  swordfs::test::meta_contract::AclRejectsMalformedLinuxEncoding(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractChunkPublicationHonorsOffTMaximum) {
  swordfs::test::meta_contract::ChunkPublicationValidatesMaximumFileExtent(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ContractReadlinkAndReclaimValidateMissingState) {
  swordfs::test::meta_contract::ReadlinkAndReclaimValidateMissingState(static_cast<IMetaEngine &>(*impl_));
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentNameCreationAcrossIndependentEnginesHasOneWinner) {
  auto peer = NewPeer(config_, volume_name_);
  ASSERT_NE(peer, nullptr);

  for (int attempt = 0; attempt < 4; ++attempt) {
    std::barrier gate(3);
    Status first_status;
    Status second_status;
    SwordFsInode first;
    SwordFsInode second;
    const std::string name = "concurrent-" + std::to_string(attempt);

    auto first_thread = swordfs::test::StartFiberTestThread([&] {
      folly::fibers::local<SwordFsContext>() = SwordFsContext{};
      gate.arrive_and_wait();
      first_status = impl_->Create(kRootInodeId, name, 0644, &first);
    });
    auto second_thread = swordfs::test::StartFiberTestThread([&] {
      folly::fibers::local<SwordFsContext>() = SwordFsContext{};
      gate.arrive_and_wait();
      second_status = peer->Create(kRootInodeId, name, 0644, &second);
    });
    gate.arrive_and_wait();
    first_thread.join();
    second_thread.join();

    EXPECT_TRUE((first_status.ok() && second_status.ToErrno() == EEXIST) ||
                (second_status.ok() && first_status.ToErrno() == EEXIST))
        << "first=" << first_status.message() << " second=" << second_status.message();

    SwordFsInode found;
    ASSERT_TRUE(impl_->Lookup(kRootInodeId, name, &found).ok());
    EXPECT_EQ(found.ino, first_status.ok() ? first.ino : second.ino);
  }
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentRenameOverwriteHasNoCrossEngineCreateGap) {
  auto peer = NewPeer(config_, volume_name_);
  ASSERT_NE(peer, nullptr);

  ASSERT_TRUE(impl_->Create(kRootInodeId, "target", 0644, nullptr).ok());
  std::atomic<bool> stop{false};
  std::atomic<int> unexpected_creates{0};
  std::atomic<int> probe_errors{0};
  std::atomic<int> rename_errors{0};
  std::barrier gate(3);

  auto observer = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    while (!stop.load(std::memory_order_acquire)) {
      auto status = peer->Create(kRootInodeId, "target", 0644, nullptr);
      if (status.ok()) {
        unexpected_creates.fetch_add(1, std::memory_order_relaxed);
      } else if (status.ToErrno() != EEXIST) {
        probe_errors.fetch_add(1, std::memory_order_relaxed);
      }
    }
  });
  auto renamer = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    for (int i = 0; i < 24; ++i) {
      auto status = impl_->Create(kRootInodeId, "source", 0644, nullptr);
      if (status.ok()) {
        status = impl_->Rename(kRootInodeId, "source", kRootInodeId, "target", swordfs::metadata::RenameFlag::kNone);
      }
      if (!status.ok()) {
        rename_errors.fetch_add(1, std::memory_order_relaxed);
      }
    }
    stop.store(true, std::memory_order_release);
  });

  gate.arrive_and_wait();
  renamer.join();
  observer.join();
  EXPECT_EQ(rename_errors.load(), 0);
  EXPECT_EQ(probe_errors.load(), 0) << "the observer must not silently ignore backend failures";
  EXPECT_EQ(unexpected_creates.load(), 0) << "rename-overwrite exposed a transient missing target";
  SwordFsInode target;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "target", &target).ok());
  EXPECT_TRUE(impl_->GetInode(target.ino, &target).ok());
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentCrossEngineRenameOfSameSourceHasOneWinner) {
  auto peer = NewPeer(config_, volume_name_);
  ASSERT_NE(peer, nullptr);
  SwordFsInode source;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "shared-source", 0644, &source).ok());

  Status first_status;
  Status second_status;
  std::barrier gate(3);
  auto first_thread = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    first_status =
        impl_->Rename(kRootInodeId, "shared-source", kRootInodeId, "first-dest", swordfs::metadata::RenameFlag::kNone);
  });
  auto second_thread = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    second_status =
        peer->Rename(kRootInodeId, "shared-source", kRootInodeId, "second-dest", swordfs::metadata::RenameFlag::kNone);
  });
  gate.arrive_and_wait();
  first_thread.join();
  second_thread.join();
  EXPECT_TRUE((first_status.ok() && second_status.IsNotFound()) || (second_status.ok() && first_status.IsNotFound()))
      << "first=" << first_status.message() << " second=" << second_status.message();

  SwordFsInode found;
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "shared-source", &found).IsNotFound());
  const std::string winning_name = first_status.ok() ? "first-dest" : "second-dest";
  const std::string losing_name = first_status.ok() ? "second-dest" : "first-dest";
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, winning_name, &found).ok());
  EXPECT_EQ(found.ino, source.ino);
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, losing_name, &found).IsNotFound());
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ReclaimUsesLatestPublishedRevisionAfterRewrite) {
  SwordFsInode file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "latest-head", 0644, &file).ok());
  const swordfs::metadata::SwordFsChunk original{.index = 0, .revision = 1, .size = 64};
  const swordfs::metadata::SwordFsChunk replacement{.index = 0, .revision = 2, .size = 128};
  ASSERT_TRUE(impl_->CommitChunk(file.ino, std::nullopt, original).ok());
  ASSERT_TRUE(impl_->CommitChunk(file.ino, original, replacement).ok());
  ASSERT_TRUE(impl_->Unlink(kRootInodeId, "latest-head").ok());
  ASSERT_TRUE(impl_->PrepareReclaim(file.ino).ok());
  auto work = PendingReclaim(file.ino);
  ASSERT_TRUE(work.has_value());
  std::vector<swordfs::chunk::cow::COWRef> refs;
  ASSERT_TRUE(swordfs::chunk::cow::DecodeCOWReclaim(*work, swordfs::test::redis_meta::kTestChunkSize, &refs).ok());
  ASSERT_EQ(refs.size(), 1U);
  EXPECT_EQ(refs[0].descriptor, replacement);
  ASSERT_TRUE(impl_->CompleteReclaim(file.ino).ok());
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentCrossEngineExchangeKeepsBothEntries) {
  auto peer = NewPeer(config_, volume_name_);
  ASSERT_NE(peer, nullptr);
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "exchange-a", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "exchange-b", 0644, &second).ok());

  std::atomic<int> failures{0};
  std::barrier gate(3);
  auto exchange = [&](RedisMetaImpl *engine, const char *from, const char *to) {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    for (int attempt = 0; attempt < 8; ++attempt) {
      auto status = engine->Rename(kRootInodeId, from, kRootInodeId, to, swordfs::metadata::RenameFlag::kExchange);
      if (!status.ok()) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  auto first_thread = swordfs::test::StartFiberTestThread(exchange, impl_.get(), "exchange-a", "exchange-b");
  auto second_thread = swordfs::test::StartFiberTestThread(exchange, peer.get(), "exchange-b", "exchange-a");
  gate.arrive_and_wait();
  first_thread.join();
  second_thread.join();
  EXPECT_EQ(failures.load(), 0);

  SwordFsInode found_a;
  SwordFsInode found_b;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "exchange-a", &found_a).ok());
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "exchange-b", &found_b).ok());
  EXPECT_NE(found_a.ino, found_b.ino);
  EXPECT_TRUE((found_a.ino == first.ino && found_b.ino == second.ino) ||
              (found_a.ino == second.ino && found_b.ino == first.ino));
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentUnlinkAndCreateAcrossEnginesCannotLoseSuccessfulCreate) {
  auto peer = NewPeer(config_, volume_name_);
  ASSERT_NE(peer, nullptr);
  SwordFsInode old_file;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "unlink-create", 0644, &old_file).ok());

  std::barrier gate(3);
  Status unlink_status;
  Status create_status;
  SwordFsInode new_file;
  auto unlink_thread = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    unlink_status = impl_->Unlink(kRootInodeId, "unlink-create");
  });
  auto create_thread = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    create_status = peer->Create(kRootInodeId, "unlink-create", 0644, &new_file);
  });
  gate.arrive_and_wait();
  unlink_thread.join();
  create_thread.join();

  ASSERT_TRUE(unlink_status.ok()) << unlink_status.message();
  ASSERT_TRUE(create_status.ok() || create_status.ToErrno() == EEXIST) << create_status.message();
  SwordFsInode found;
  if (create_status.ok()) {
    ASSERT_TRUE(impl_->Lookup(kRootInodeId, "unlink-create", &found).ok());
    EXPECT_EQ(found.ino, new_file.ino);
    EXPECT_NE(new_file.ino, old_file.ino);
  } else {
    EXPECT_TRUE(impl_->Lookup(kRootInodeId, "unlink-create", &found).IsNotFound());
  }
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentIndependentNamesAcrossEnginesAreAllReachable) {
  auto peer = NewPeer(config_, volume_name_);
  ASSERT_NE(peer, nullptr);
  std::barrier gate(3);
  std::atomic<int> failures{0};
  auto create_many = [&](RedisMetaImpl *engine, const char *prefix) {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    for (int i = 0; i < 20; ++i) {
      auto status = engine->Create(kRootInodeId, std::string(prefix) + std::to_string(i), 0644, nullptr);
      if (!status.ok()) {
        failures.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };
  auto first_thread = swordfs::test::StartFiberTestThread(create_many, impl_.get(), "first-");
  auto second_thread = swordfs::test::StartFiberTestThread(create_many, peer.get(), "second-");
  gate.arrive_and_wait();
  first_thread.join();
  second_thread.join();
  EXPECT_EQ(failures.load(), 0);
  for (int i = 0; i < 20; ++i) {
    SwordFsInode first;
    SwordFsInode second;
    ASSERT_TRUE(impl_->Lookup(kRootInodeId, "first-" + std::to_string(i), &first).ok());
    ASSERT_TRUE(impl_->Lookup(kRootInodeId, "second-" + std::to_string(i), &second).ok());
    EXPECT_NE(first.ino, second.ino);
  }
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

FIBER_TEST_F(RedisMetaImplTest, ConcurrentRenamesToSameDestinationKeepOneLiveWinner) {
  auto peer = NewPeer(config_, volume_name_);
  ASSERT_NE(peer, nullptr);
  SwordFsInode first;
  SwordFsInode second;
  ASSERT_TRUE(impl_->Create(kRootInodeId, "move-first", 0644, &first).ok());
  ASSERT_TRUE(impl_->Create(kRootInodeId, "move-second", 0644, &second).ok());

  std::barrier gate(3);
  Status first_status;
  Status second_status;
  auto first_thread = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    first_status =
        impl_->Rename(kRootInodeId, "move-first", kRootInodeId, "shared-dest", swordfs::metadata::RenameFlag::kNone);
  });
  auto second_thread = swordfs::test::StartFiberTestThread([&] {
    folly::fibers::local<SwordFsContext>() = SwordFsContext{};
    gate.arrive_and_wait();
    second_status =
        peer->Rename(kRootInodeId, "move-second", kRootInodeId, "shared-dest", swordfs::metadata::RenameFlag::kNone);
  });
  gate.arrive_and_wait();
  first_thread.join();
  second_thread.join();

  ASSERT_TRUE(first_status.ok()) << first_status.message();
  ASSERT_TRUE(second_status.ok()) << second_status.message();
  SwordFsInode found;
  ASSERT_TRUE(impl_->Lookup(kRootInodeId, "shared-dest", &found).ok());
  EXPECT_TRUE(found.ino == first.ino || found.ino == second.ino);
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "move-first", &found).IsNotFound());
  EXPECT_TRUE(impl_->Lookup(kRootInodeId, "move-second", &found).IsNotFound());
  swordfs::test::RunInTestThreadFromFiber([&] { peer.reset(); });
}

#ifndef NDEBUG
TEST_F(RedisMetaImplTest, ContractRuntimeApiRejectsNonFiberCaller) {
  SwordFsInode file;
  EXPECT_DEATH(
      { (void)impl_->GetInode(kRootInodeId, &file); },
      "execution-domain violation at .*expected=fiber, actual=POSIX-thread");
}
#endif

}  // namespace
