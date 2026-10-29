// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Verify durable orphan handoff without simulating a retired physical GC.
// Slice cleanup is deferred; no production code can physically delete Slice
// objects until #273 defines its own reachability and reader-lifetime rules.

#include <fcntl.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "FiberTest.hpp"
#include "RedisTestVolumeUtils.hpp"
#include "UnsupportedMetaEngine.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/ChunkTestSupport.hpp"
#include "metadata/IMetaEngine.hpp"
#include "utils/Status.hpp"
#include "vfs/InodeHandle.hpp"
#include "vfs/OrphanReclaimer.hpp"
#include "volume/VolumeImpl.hpp"

namespace swordfs::vfs {
namespace {

using swordfs::metadata::InodeID;
using swordfs::metadata::kRootInodeId;
using swordfs::metadata::SwordFsInode;
using swordfs::metadata::SwordFsVolume;
using swordfs::test::UnsupportedMetaEngine;
using swordfs::utils::Status;

class EmptyReclaimerMetaEngine : public UnsupportedMetaEngine {
 public:
  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingReclaims(const metadata::ReclaimVisitorFn &) override {
    return Status::OK();
  }
  Status VisitPendingDeletesBatch(size_t, const metadata::PendingDeleteVisitorFn &, bool *has_more) override {
    if (has_more != nullptr) {
      *has_more = false;
    }
    return Status::OK();
  }
};

// The orphan reclaimer needs a typed Slice mount, but no physical GC policy.
class SliceReclaimerMetaEngine : public EmptyReclaimerMetaEngine {
 public:
  Status OpenChunkMetadata(metadata::ChunkType type, metadata::ChunkMetadataPtr *out) override {
    if (out == nullptr || type != metadata::ChunkType::kChunkSlice) {
      return Status::InvalidArgument("expected Slice test metadata capability");
    }
    *out = std::make_shared<swordfs::test::StubChunkMetadata>(type);
    return Status::OK();
  }
};

class ReclaimerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SwordFsVolume config;
    config.chunk_type = metadata::ChunkType::kChunkSlice;
    auto meta = CreateMetaEngine(&config);
    ASSERT_NE(meta, nullptr);
    meta_ = meta.get();
    auto data = std::make_unique<swordfs::test::chunk_test::NullDataEngine>();
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), std::move(data), std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    // A test that started the reclaim worker must never let it outlive the
    // engines it walks; Stop() is idempotent and safe when never started.
    OrphanReclaimer::Instance().Stop();
    // Drop per-inode runtime state and the injected engines on the threads
    // that own them.
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  virtual std::unique_ptr<metadata::IMetaEngine> CreateMetaEngine(SwordFsVolume *) {
    return std::make_unique<swordfs::test::ConfiguredMetaEngine<SliceReclaimerMetaEngine>>();
  }

  std::vector<InodeID> OrphanCandidates() {
    std::vector<InodeID> out;
    auto status = meta_->VisitOrphanCandidates([&out](InodeID ino) {
      out.push_back(ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    std::sort(out.begin(), out.end());
    return out;
  }

  std::vector<InodeID> PendingReclaims() {
    std::vector<InodeID> out;
    auto status = meta_->VisitPendingReclaims([&out](const metadata::ReclaimWork &work) {
      out.push_back(work.ino);
      return Status::OK();
    });
    EXPECT_TRUE(status.ok()) << status.message();
    std::sort(out.begin(), out.end());
    return out;
  }

  metadata::IMetaEngine *meta_ = nullptr;
};

class ReclaimerRedisTest : public ReclaimerTest {
 protected:
  void SetUp() override {
    if (std::getenv("SWORDFS_REDIS_TEST_URL") == nullptr) {
      GTEST_SKIP() << "Redis metadata service is not configured";
    }
    ReclaimerTest::SetUp();
  }

  std::unique_ptr<metadata::IMetaEngine> CreateMetaEngine(SwordFsVolume *config) override {
    std::unique_ptr<metadata::RedisMetaImpl> engine;
    const auto status = swordfs::test::MakeFormattedRedisMetaEngine("reclaimer", config, &engine);
    EXPECT_TRUE(status.ok()) << status.message();
    return engine;
  }
};

class ReclaimerNoEngineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    volume::VolumeImpl::Initialize();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    OrphanReclaimer::Instance().Stop();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }
};

FIBER_TEST_F(ReclaimerNoEngineTest, ReconcileWithoutEnginesIsANoOp) {
  EXPECT_TRUE(OrphanReclaimer::Instance().Reconcile().ok());
}

FIBER_TEST_F(ReclaimerNoEngineTest, MissingDataEngineFailsClosedAndReconcileStaysHarmless) {
  swordfs::test::RunInTestThreadFromFiber([&] {
    SwordFsVolume config;
    config.chunk_type = metadata::ChunkType::kChunkSlice;
    auto configured = std::make_unique<swordfs::test::ConfiguredMetaEngine<SliceReclaimerMetaEngine>>();
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(configured), nullptr, std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  });
  ASSERT_NE(volume::VolumeImpl::Instance().meta_engine(), nullptr);
  EXPECT_TRUE(OrphanReclaimer::Instance().Reconcile().ok());
}

class FailingScanMetaEngine : public SliceReclaimerMetaEngine {
 public:
  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &visitor) override {
    if (!orphan_scan_status_.ok()) {
      return orphan_scan_status_;
    }
    if (candidate_ino_.has_value()) {
      return visitor(*candidate_ino_);
    }
    return Status::OK();
  }

  Status PrepareReclaim(InodeID) override {
    return prepare_reclaim_status_;
  }

  void SetCandidate(InodeID ino) {
    candidate_ino_ = ino;
  }

  Status orphan_scan_status_ = Status::OK();
  Status prepare_reclaim_status_ = Status::OK();

 private:
  std::optional<InodeID> candidate_ino_;
};

class ReclaimerScanFailureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<FailingScanMetaEngine>>();
    meta_ = meta.get();
    SwordFsVolume config;
    config.chunk_type = metadata::ChunkType::kChunkSlice;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), nullptr, std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
  }

  void TearDown() override {
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  FailingScanMetaEngine *meta_ = nullptr;
};

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileSurfacesOrphanScanFailure) {
  meta_->orphan_scan_status_ = Status::IOError("orphan scan unavailable");
  const auto status = OrphanReclaimer::Instance().Reconcile();
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
  EXPECT_EQ(status.message(), "orphan scan unavailable");
}

FIBER_TEST_F(ReclaimerScanFailureTest, ReconcileAggregatesPrepareReclaimFailure) {
  meta_->SetCandidate(42);
  meta_->prepare_reclaim_status_ = Status::IOError("prepare reclaim unavailable");
  const auto status = OrphanReclaimer::Instance().Reconcile();
  EXPECT_EQ(status.ToErrno(), EIO);
  EXPECT_EQ(status.message(), "orphan reclaim left 1 inode(s) pending");
}

FIBER_TEST_F(ReclaimerRedisTest, ReconcileLeavesLargeOrphanBacklogsForLaterPassesAndEventuallyDrainsThem) {
  constexpr int kCandidates = 300;
  for (int i = 0; i < kCandidates; ++i) {
    SwordFsInode file;
    const std::string name = "orphan-batch-" + std::to_string(i);
    ASSERT_TRUE(meta_->Create(kRootInodeId, name, 0644, &file).ok());
    ASSERT_TRUE(meta_->Unlink(kRootInodeId, name).ok());
  }
  ASSERT_EQ(OrphanCandidates().size(), static_cast<size_t>(kCandidates));

  ASSERT_TRUE(OrphanReclaimer::Instance().Reconcile().ok());
  const auto after_first_pass = OrphanCandidates();
  EXPECT_FALSE(after_first_pass.empty()) << "one reconciliation pass must not monopolize an unbounded orphan backlog";
  EXPECT_LT(after_first_pass.size(), static_cast<size_t>(kCandidates))
      << "a bounded pass must still make forward progress";

  for (int pass = 0; pass < 10 && !OrphanCandidates().empty(); ++pass) {
    ASSERT_TRUE(OrphanReclaimer::Instance().Reconcile().ok());
  }
  EXPECT_TRUE(OrphanCandidates().empty()) << "repeated bounded passes must converge on the durable backlog";
  EXPECT_EQ(PendingReclaims().size(), static_cast<size_t>(kCandidates));
}

class SlowOrphanScanMetaEngine : public UnsupportedMetaEngine {
 public:
  Status VisitOrphanCandidates(const metadata::InodeVisitorFn &visitor) override {
    constexpr int kCandidates = 1000;
    for (int i = 0; i < kCandidates; ++i) {
      scan_started_.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      auto status = visitor(static_cast<InodeID>(1000 + i));
      if (!status.ok()) {
        return status;
      }
    }
    return Status::OK();
  }

  Status PrepareReclaim(InodeID) override {
    return Status::OK();
  }

  bool scan_started() const {
    return scan_started_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<bool> scan_started_{false};
};

class ReclaimerSlowScanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto meta = std::make_unique<swordfs::test::ConfiguredMetaEngine<SlowOrphanScanMetaEngine>>();
    meta_ = meta.get();
    SwordFsVolume config;
    config.chunk_type = metadata::ChunkType::kChunkSlice;
    const auto status = swordfs::test::LoadTestVolumeRuntime(std::move(meta), nullptr, std::move(config));
    ASSERT_TRUE(status.ok()) << status.message();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
  }

  void TearDown() override {
    OrphanReclaimer::Instance().Stop();
    swordfs::test::RunInTestFiber([&] { InodeHandleManager::Instance().Initialize(); });
    volume::VolumeImpl::Initialize();
  }

  SlowOrphanScanMetaEngine *meta_ = nullptr;
};

TEST_F(ReclaimerSlowScanTest, OrphanWorkerStopInterruptsALongCandidateScan) {
  auto &worker = OrphanReclaimer::Instance();
  worker.Start();

  const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < start_deadline && !meta_->scan_started()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(meta_->scan_started()) << "the worker must enter the orphan scan before shutdown is measured";

  const auto started = std::chrono::steady_clock::now();
  worker.Stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;

  // A mount with a large durable orphan backlog must still tear down promptly.
  // Stop should interrupt candidate visitation instead of joining an entire
  // backlog scan before the daemon can exit.
  EXPECT_LT(elapsed, std::chrono::milliseconds(250));
}

TEST_F(ReclaimerTest, OrphanWorkerLifecycleIsIdempotent) {
  auto &worker = OrphanReclaimer::Instance();
  const auto started = std::chrono::steady_clock::now();
  worker.Stop();
  worker.Start();
  worker.Start();
  worker.Wake();
  worker.Wake();
  worker.Stop();
  worker.Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(500));
}

}  // namespace
}  // namespace swordfs::vfs
