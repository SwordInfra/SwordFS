// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "FiberTest.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "chunk/cow/COWCleanup.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/ChunkSizePlan.hpp"
#include "metadata/cow/COWChunkMetadata.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/mem/VolumeFile.hpp"
#include "storage/IDataEngine.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkType;
using swordfs::metadata::SwordFsVolume;
using swordfs::metadata::mem::VolumeFile;
using swordfs::utils::Status;
using swordfs::volume::FormatOptions;
using swordfs::volume::MountOptions;
using swordfs::volume::VolumeImpl;

namespace {

void ParseConfig(std::vector<std::string> args) {
  CLI::App app{"SwordFS volume adapter test"};
  auto &config = swordfs::config::ConfigCenter::Instance();
  config.ConfigureOptions(app);
  std::vector<const char *> argv;
  argv.reserve(args.size());
  for (const auto &arg : args) {
    argv.push_back(arg.c_str());
  }
  app.parse(static_cast<int>(argv.size()), argv.data());
}

class NoopDataEngine final : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view) override {
    return Status::OK();
  }
};

class TrackedCleanupDataEngine final : public swordfs::storage::IDataEngine {
 public:
  Status Initialize() override {
    return Status::OK();
  }
  Status Put(std::string_view, std::unique_ptr<folly::IOBuf>) override {
    return Status::OK();
  }
  Status Get(std::string_view, size_t, size_t, folly::IOBuf *) override {
    return Status::OK();
  }
  Status Delete(std::string_view key) override {
    std::lock_guard lock(mutex_);
    deleted_.emplace_back(key);
    return Status::OK();
  }
  std::vector<std::string> Deleted() const {
    std::lock_guard lock(mutex_);
    return deleted_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> deleted_;
};

class InvalidChunkMetadataEngine final : public swordfs::metadata::MemMetaImpl {
 public:
  enum class Result {
    kNullCapability,
    kMismatchedChunkType,
    kCowTypeButWrongClass,
  };

  explicit InvalidChunkMetadataEngine(Result result) : result_(result) {
  }

  Status LoadVolume(SwordFsVolume *out) override {
    return swordfs::test::LoadConfiguredTestVolume(out);
  }

  Status OpenChunkMetadata(ChunkType, swordfs::metadata::ChunkMetadataPtr *out) override {
    if (out == nullptr) {
      return Status::InvalidArgument("chunk metadata output is null");
    }
    if (result_ == Result::kNullCapability) {
      out->reset();
    } else if (result_ == Result::kMismatchedChunkType) {
      *out = std::make_shared<swordfs::test::StubChunkMetadata>(ChunkType::kChunkSlice);
    } else {
      *out = std::make_shared<swordfs::test::StubChunkMetadata>(ChunkType::kCow);
    }
    return Status::OK();
  }

 private:
  Result result_;
};

}  // namespace

TEST(VolumeImplConfigAdapterTest, CreateFromUsesParsedFormatConfiguration) {
  swordfs::test::RegisterTestVolumeEngines();
  ParseConfig({
      "swordfs",
      "format",
      "--volume",
      "adaptervol",
      "--meta",
      "swordfs-test-meta://local",
      "--bucket",
      "swordfs-test-data://endpoint/bucket",
      "--storage-region",
      "adapter-region",
      "--chunk-size",
      "4096",
      "--chunk-type",
      "redis_cache",
  });

  VolumeImpl volume;
  const auto status = volume.CreateFrom(swordfs::config::ConfigCenter::Instance());
  EXPECT_EQ(status.ToErrno(), ENOSYS);
  EXPECT_EQ(volume.config().name, "adaptervol");
  EXPECT_EQ(volume.config().bucket, "swordfs-test-data://endpoint/bucket");
  EXPECT_EQ(volume.config().region, "adapter-region");
  EXPECT_EQ(volume.config().chunk_size, 4096U);
  EXPECT_EQ(volume.config().chunk_type, ChunkType::kRedisCache);
}

TEST(VolumeImplConfigAdapterTest, CreateFromMapsParsedPosixAclFeature) {
  swordfs::test::RegisterTestVolumeEngines();
  ParseConfig({
      "swordfs",
      "format",
      "--volume",
      "acladaptervol",
      "--meta",
      "swordfs-test-meta://local",
      "--bucket",
      "swordfs-test-data://endpoint/bucket",
      "--enable-posix-acl",
  });

  VolumeImpl volume;
  // This adapter test intentionally does not prepare a concrete metadata
  // engine. The configuration projection happens before backend creation and
  // is the behavior under test; canonical persistence is covered separately
  // by SwordFsVolume serialization tests.
  EXPECT_FALSE(volume.CreateFrom(swordfs::config::ConfigCenter::Instance()).ok());
  EXPECT_TRUE(volume.config().enable_posix_acl);
}

TEST(VolumeImplConfigAdapterTest, RejectsLegacyChunkSelectionOptions) {
  for (const auto *legacy_option : {"--chunk-overwrite-strategy", "--chunk-overwrite-mechanism"}) {
    EXPECT_THROW(ParseConfig({
                     "swordfs",
                     "format",
                     "--volume",
                     "legacy-option",
                     "--meta",
                     "swordfs-test-meta://local",
                     "--bucket",
                     "swordfs-test-data://endpoint/bucket",
                     legacy_option,
                     "cow",
                 }),
                 CLI::ParseError);
  }
}

TEST(VolumeImplConfigAdapterTest, LoadFromUsesParsedMountRuntimeConfiguration) {
  swordfs::test::RegisterTestVolumeEngines();
  swordfs::test::pending_volume = SwordFsVolume{
      .name = "adaptermount",
      .storage = std::string(swordfs::test::kTestDataEngine),
      .bucket = "opaque://endpoint/bucket",
      .region = "persisted-region",
  };
  swordfs::test::pending_meta_engine =
      std::make_unique<swordfs::test::ConfiguredMetaEngine<swordfs::metadata::MemMetaImpl>>();
  swordfs::test::pending_data_engine = std::make_unique<NoopDataEngine>();
  ParseConfig({
      "swordfs",
      "mount",
      "--volume",
      "adaptermount",
      "--meta",
      "swordfs-test-meta://local",
      "--storage-thread-count",
      "7",
      "/tmp/swordfs-adapter-mount",
  });

  VolumeImpl volume;
  const auto status = volume.LoadFrom(swordfs::config::ConfigCenter::Instance());
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(swordfs::test::pending_data_options.location, "opaque://endpoint/bucket");
  EXPECT_EQ(swordfs::test::pending_data_options.region, "persisted-region");
  EXPECT_EQ(swordfs::test::pending_data_options.worker_count, 7U);
}

TEST(VolumeImplConfigAdapterTest, CreateFromRejectsUnknownChunkTypeName) {
  swordfs::test::RegisterTestVolumeEngines();
  ParseConfig({
      "swordfs",
      "format",
      "--volume",
      "unknownmechanism",
      "--meta",
      "swordfs-test-meta://local",
      "--bucket",
      "swordfs-test-data://endpoint/bucket",
      "--chunk-type",
      "unknown-type",
  });

  VolumeImpl volume;
  EXPECT_EQ(volume.CreateFrom(swordfs::config::ConfigCenter::Instance()).ToErrno(), EINVAL);
}

class VolumeImplTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (::mkdir("/etc/swordfs", 0755) != 0 && errno != EEXIST) {
      FAIL() << "failed to create /etc/swordfs: " << strerror(errno);
    }
    tmpdir_ = "/tmp/swordfs_volimpl_test_" + std::to_string(::getpid());
    std::system(("mkdir -p " + tmpdir_).c_str());
  }
  void TearDown() override {
    std::system(("rm -rf " + tmpdir_).c_str());
  }

  std::string makeVolumeName(const std::string &vol_name) const {
    const auto *test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string test_name = test_info != nullptr ? test_info->name() : "unknown";
    const auto sanitize = [](std::string_view value) {
      std::string out;
      for (const unsigned char ch : value) {
        if (std::isalnum(ch)) {
          out.push_back(static_cast<char>(ch));
        }
      }
      if (out.empty() || !std::isalpha(static_cast<unsigned char>(out.front()))) {
        out.insert(out.begin(), 'v');
      }
      return out;
    };
    return sanitize(vol_name) + sanitize(test_name) + std::to_string(::getpid());
  }

  FormatOptions makeFormatOptions(const std::string &meta_url, const std::string &vol_name = "testvol",
                                  const std::string &bucket_url = "", const std::string &storage_region = "",
                                  ChunkType chunk_type = ChunkType::kCow) const {
    return FormatOptions{
        .name = makeVolumeName(vol_name),
        .meta_url = meta_url,
        .bucket = bucket_url,
        .region = storage_region,
        .chunk_type = chunk_type,
    };
  }

  MountOptions makeMountOptions(const std::string &meta_url, const std::string &vol_name = "testvol") const {
    return MountOptions{
        .name = makeVolumeName(vol_name),
        .meta_url = meta_url,
    };
  }

  std::string tmpdir_;
};

#ifndef NDEBUG
TEST(VolumeImplDomainTest, LifecycleRejectsFiberCaller) {
  EXPECT_DEATH(
      { swordfs::test::RunInTestFiber([] { VolumeImpl::Initialize(); }); },
      "execution-domain violation at .*expected=POSIX-thread, actual=fiber");
}
#endif

// ── CreateFrom ──────────────────────────────────────────────────────

TEST_F(VolumeImplTest, CreateFromSucceeds) {
  auto options = makeFormatOptions("memory://local");
  VolumeImpl vol;
  const auto status = vol.CreateFrom(options);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(vol.config().chunk_type, ChunkType::kCow);
}

TEST_F(VolumeImplTest, CreateFromRejectsInvalidBucketUrl) {
  auto options = makeFormatOptions("memory://local", "testvol", "not-a-storage-url");
  VolumeImpl volume;

  const auto status = volume.CreateFrom(options);

  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();
}

TEST_F(VolumeImplTest, CreateFromRejectsInvalidMetadataUrl) {
  auto options = makeFormatOptions("not-a-metadata-url");
  VolumeImpl volume;

  const auto status = volume.CreateFrom(options);

  EXPECT_EQ(status.ToErrno(), EINVAL) << status.message();
}

TEST_F(VolumeImplTest, FormatRejectsUnimplementedChunkType) {
  auto options =
      makeFormatOptions("memory://local", "testvol", "s3://endpoint.example.com/bucket", "", ChunkType::kRedisCache);
  VolumeImpl vol;
  const auto status = vol.CreateFrom(options);
  EXPECT_TRUE(status.ToErrno() == ENOSYS) << status.message();
  EXPECT_FALSE(VolumeFile{options.name}.Exists());
}

TEST_F(VolumeImplTest, MountUsesPersistedChunkTypeAndRejectsUnimplementedType) {
  auto format_options = makeFormatOptions("memory://local");
  VolumeImpl formatted;
  const auto format_status = formatted.CreateFrom(format_options);
  ASSERT_TRUE(format_status.ok()) << format_status.message();

  VolumeImpl mounted;
  auto mount_options = makeMountOptions("memory://local");
  ASSERT_TRUE(mounted.LoadFrom(mount_options).ok());
  ASSERT_NE(mounted.chunk_factory(), nullptr);
  EXPECT_EQ(mounted.config().chunk_type, ChunkType::kCow);

  SwordFsVolume stored = mounted.config();
  stored.chunk_type = ChunkType::kRedisCache;
  const auto unsupported_options = makeMountOptions("memory://local");
  stored.name = unsupported_options.name;
  ASSERT_TRUE(VolumeFile{stored.name}.Write(stored).ok());
  VolumeImpl unsupported;
  EXPECT_TRUE(unsupported.LoadFrom(unsupported_options).ToErrno() == ENOSYS);
}

TEST_F(VolumeImplTest, MountRejectsInvalidChunkMetadataCapability) {
  swordfs::test::RegisterTestVolumeEngines();
  for (const auto result : {InvalidChunkMetadataEngine::Result::kNullCapability,
                            InvalidChunkMetadataEngine::Result::kMismatchedChunkType}) {
    SCOPED_TRACE(static_cast<int>(result));
    const auto volume_name = makeVolumeName("invalid-chunk-metadata");
    swordfs::test::pending_volume = SwordFsVolume{
        .name = volume_name,
        .chunk_type = ChunkType::kCow,
    };
    swordfs::test::pending_meta_engine = std::make_unique<InvalidChunkMetadataEngine>(result);
    swordfs::test::pending_data_engine.reset();

    VolumeImpl volume;
    const auto status = volume.LoadFrom(swordfs::test::MakeTestMountOptions(volume_name));
    EXPECT_EQ(status.ToErrno(), EINVAL);
    EXPECT_EQ(status.message(), "chunk metadata type mismatch");
  }
}

TEST_F(VolumeImplTest, MountRejectsCleanupMetadataClassMismatch) {
  swordfs::test::RegisterTestVolumeEngines();
  const auto volume_name = makeVolumeName("invalid-cleanup-metadata");
  swordfs::test::pending_volume = SwordFsVolume{
      .name = volume_name,
      .storage = std::string(swordfs::test::kTestDataEngine),
      .bucket = "opaque://endpoint/bucket",
      .chunk_type = ChunkType::kCow,
  };
  swordfs::test::pending_meta_engine =
      std::make_unique<InvalidChunkMetadataEngine>(InvalidChunkMetadataEngine::Result::kCowTypeButWrongClass);
  swordfs::test::pending_data_engine = std::make_unique<NoopDataEngine>();

  VolumeImpl volume;
  const auto status = volume.LoadFrom(swordfs::test::MakeTestMountOptions(volume_name));
  EXPECT_EQ(status.ToErrno(), EINVAL);
  EXPECT_EQ(status.message(), "COW cleanup metadata type mismatch");
}

TEST_F(VolumeImplTest, RuntimeGcProbeDeletesOrphansButPreservesAttachedChunkIDs) {
  swordfs::test::RegisterTestVolumeEngines();
  const auto name = makeVolumeName("live-gc-reachability");
  const SwordFsVolume config{
      .name = name,
      .storage = std::string(swordfs::test::kTestDataEngine),
      .bucket = "opaque://endpoint/bucket",
      .chunk_size = 4096,
      .chunk_type = ChunkType::kCow,
  };
  ASSERT_TRUE(VolumeFile{name}.Write(config).ok());
  auto data = std::make_unique<TrackedCleanupDataEngine>();
  auto *raw_data = data.get();
  swordfs::test::pending_data_engine = std::move(data);

  VolumeImpl volume;
  ASSERT_TRUE(volume.LoadFrom({.name = name, .meta_url = "memory://local"}).ok());
  auto *raw_meta = dynamic_cast<swordfs::metadata::MemMetaImpl *>(volume.meta_engine());
  ASSERT_NE(raw_meta, nullptr);
  swordfs::metadata::ChunkMetadataPtr capability;
  ASSERT_TRUE(volume.meta_engine()->OpenChunkMetadata(ChunkType::kCow, &capability).ok());
  auto cow = std::dynamic_pointer_cast<swordfs::metadata::cow::COWChunkMetadata>(capability);
  ASSERT_NE(cow, nullptr);

  swordfs::metadata::ChunkID detached_id;
  swordfs::metadata::ChunkID live_id;
  swordfs::metadata::ChunkID orphan_id;
  swordfs::test::RunInTestFiber([&] {
    ASSERT_TRUE(cow->AllocateChunkID(&detached_id).ok());
    ASSERT_TRUE(cow->AllocateChunkID(&live_id).ok());
    ASSERT_TRUE(cow->AllocateChunkID(&orphan_id).ok());
    for (const auto id : {detached_id, live_id, orphan_id}) {
      swordfs::metadata::cow::COWChunkRevision revision;
      ASSERT_TRUE(cow->AllocateRevision(id, &revision).ok());
      ASSERT_TRUE(cow->CompareExchangeHead(id, std::nullopt, {.revision = revision, .size = 8}).ok());
    }
  });

  swordfs::metadata::SwordFsInode file;
  swordfs::test::RunInTestFiber([&] {
    ASSERT_TRUE(raw_meta->Create(swordfs::metadata::kRootInodeId, "attached", 0644, &file).ok());
    ASSERT_TRUE(raw_meta->AttachPrepared(file.ino, 0, detached_id, 8, {.eof = 0}).ok());

    swordfs::metadata::FileMappingSnapshot snapshot;
    ASSERT_TRUE(raw_meta->ReadFileMappingSnapshot(file.ino, &snapshot).ok());
    swordfs::metadata::ChunkSizePlan shrink;
    ASSERT_TRUE(swordfs::metadata::PlanChunkSizeChange(8, 0, 4096, snapshot.mappings, &shrink).ok());
    swordfs::metadata::SwordFsAttr requested;
    requested.size = 0;
    swordfs::metadata::ChunkSizeCommitResult committed;
    ASSERT_TRUE(
        raw_meta->CommitShrink(file.ino, shrink, requested, swordfs::metadata::SetAttrField::kSize, &committed).ok());
    // A new identity may materialize after a detach. Cleanup for the old
    // identity must not touch this still-attached successor.
    ASSERT_TRUE(raw_meta->AttachPrepared(file.ino, 0, live_id, 8, {.eof = 0}).ok());

    swordfs::metadata::SwordFsInode orphan;
    ASSERT_TRUE(raw_meta->Create(swordfs::metadata::kRootInodeId, "orphan", 0644, &orphan).ok());
    ASSERT_TRUE(raw_meta->AttachPrepared(orphan.ino, 0, orphan_id, 8, {.eof = 0}).ok());
    ASSERT_TRUE(raw_meta->Unlink(swordfs::metadata::kRootInodeId, "orphan").ok());
    ASSERT_TRUE(raw_meta->PrepareReclaim(orphan.ino).ok());
  });

  volume.StartRuntimeServices();
  bool pending_empty = false;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  do {
    swordfs::test::RunInTestFiber([&] {
      bool has_more = false;
      size_t count = 0;
      ASSERT_TRUE(raw_meta
                      ->VisitPendingDeletesBatch(
                          32,
                          [&](const swordfs::metadata::PendingDelete &) {
                            ++count;
                            return Status::OK();
                          },
                          &has_more)
                      .ok());
      pending_empty = count == 0 && !has_more;
    });
    if (!pending_empty) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  } while (!pending_empty && std::chrono::steady_clock::now() < deadline);
  volume.StopRuntimeServices();

  ASSERT_TRUE(pending_empty) << "production GC should acknowledge both live and orphan candidates";
  const auto deleted = raw_data->Deleted();
  EXPECT_EQ(deleted.size(), 2U);
  const auto was_deleted = [&](swordfs::metadata::ChunkID id) {
    return std::find(deleted.begin(), deleted.end(), std::to_string(id.Value()) + "/1") != deleted.end();
  };
  EXPECT_TRUE(was_deleted(detached_id));
  EXPECT_TRUE(was_deleted(orphan_id));
  EXPECT_FALSE(was_deleted(live_id));
  swordfs::test::RunInTestFiber([&] {
    swordfs::metadata::cow::COWChunkHead head;
    EXPECT_TRUE(cow->GetHead(live_id, &head).ok());
    EXPECT_TRUE(cow->GetHead(orphan_id, &head).IsNotFound());
  });
}

TEST_F(VolumeImplTest, CreateFromNormalizesDataEngineIdentity) {
  auto options = makeFormatOptions("memory://local", "testvol", "S3://endpoint.example.com/bucket");
  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(options).ok());
  EXPECT_EQ(vol.config().storage, "s3");
}

TEST_F(VolumeImplTest, CreateFromRedisEngine) {
  const char *redis_url = std::getenv("SWORDFS_REDIS_TEST_URL");
  if (redis_url == nullptr) {
    GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
  }
  auto format_options =
      makeFormatOptions(redis_url, "redis-" + tmpdir_, "s3://endpoint.example.com/bucket", "us-east-1");

  VolumeImpl::Initialize();
  Status status = VolumeImpl::Instance().CreateFrom(format_options);
  ASSERT_TRUE(status.ok()) << status.message();

  auto mount_options = makeMountOptions(redis_url, "redis-" + tmpdir_);
  VolumeImpl::Initialize();
  status = VolumeImpl::Instance().LoadFrom(mount_options);
  EXPECT_TRUE(status.ok()) << status.message();
  EXPECT_FALSE(VolumeFile{tmpdir_}.Exists());

  VolumeImpl::Initialize();
  status = VolumeImpl::Instance().CreateFrom(format_options);
  EXPECT_TRUE(status.ToErrno() == EEXIST) << status.message();
}

TEST_F(VolumeImplTest, LoadFromS3Engine) {
  auto format_options = makeFormatOptions("memory://local", "testvol", "s3://myhost.example.com/mybucket", "us-west-2");

  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(format_options).ok());

  VolumeImpl::Initialize();
  auto mount_options = makeMountOptions("memory://local", "testvol");
  Status st = VolumeImpl::Instance().LoadFrom(mount_options);
  ASSERT_TRUE(st.ok()) << st.message();
  VolumeImpl::Instance().Shutdown();
}

TEST_F(VolumeImplTest, LoadFromUnknownDataEngine) {
  auto options = makeMountOptions("memory://local", "testvol");
  SwordFsVolume stored;
  stored.name = options.name;
  stored.storage = "does-not-exist";
  stored.bucket = "s3://endpoint.example.com/bucket";
  ASSERT_TRUE(VolumeFile{options.name}.Write(stored).ok());

  VolumeImpl vol;
  Status st = vol.LoadFrom(options);
  EXPECT_TRUE(st.ToErrno() == ENOSYS) << st.message();
}

TEST_F(VolumeImplTest, LoadFromUsesPersistedDataEngineIdentity) {
  auto options = makeMountOptions("memory://local", "testvol");
  SwordFsVolume stored;
  stored.name = options.name;
  stored.storage = "does-not-exist";
  stored.bucket = "s3://endpoint.example.com/bucket";
  ASSERT_TRUE(VolumeFile{options.name}.Write(stored).ok());

  VolumeImpl vol;
  Status st = vol.LoadFrom(options);
  EXPECT_TRUE(st.ToErrno() == ENOSYS) << st.message();
}

TEST_F(VolumeImplTest, LoadFromRejectsMissingDataEngineIdentity) {
  auto options = makeMountOptions("memory://local", "testvol");
  SwordFsVolume stored;
  stored.name = options.name;
  stored.bucket = "s3://endpoint.example.com/bucket";
  ASSERT_TRUE(VolumeFile{options.name}.Write(stored).ok());

  VolumeImpl vol;
  Status st = vol.LoadFrom(options);
  EXPECT_TRUE(st.ToErrno() == EIO) << st.message();
}

TEST_F(VolumeImplTest, LoadFromRejectsMissingDataEngineLocation) {
  auto options = makeMountOptions("memory://local", "testvol");
  SwordFsVolume stored;
  stored.name = options.name;
  stored.storage = "s3";
  ASSERT_TRUE(VolumeFile{options.name}.Write(stored).ok());

  VolumeImpl vol;
  Status status = vol.LoadFrom(options);
  EXPECT_TRUE(status.ToErrno() == EIO) << status.message();
}

TEST_F(VolumeImplTest, LoadFromS3UrlMissingBucketName) {
  auto options = makeMountOptions("memory://local", "testvol");
  SwordFsVolume stored;
  stored.name = options.name;
  stored.storage = "s3";
  stored.bucket = "s3://endpoint.example.com";
  ASSERT_TRUE(VolumeFile{options.name}.Write(stored).ok());

  VolumeImpl vol;
  Status st = vol.LoadFrom(options);
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("missing bucket name"), std::string::npos) << st.message();
}

TEST_F(VolumeImplTest, CreateFromVolumeAlreadyExists) {
  auto options = makeFormatOptions("memory://local", tmpdir_);
  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(options).ok());

  // Second format on the same path must fail.
  VolumeImpl vol2;
  Status st = vol2.CreateFrom(options);
  EXPECT_FALSE(st.ok());
}

// ── LoadFrom ────────────────────────────────────────────────────────

TEST_F(VolumeImplTest, LoadFromSucceeds) {
  auto format_options = makeFormatOptions("memory://local", tmpdir_);
  VolumeImpl vol;
  ASSERT_TRUE(vol.CreateFrom(format_options).ok());

  VolumeImpl vol2;
  auto mount_options = makeMountOptions("memory://local", tmpdir_);
  Status st = vol2.LoadFrom(mount_options);
  EXPECT_TRUE(st.ok()) << st.message();
}

TEST_F(VolumeImplTest, LoadFromUnsupportedEngine) {
  auto options = makeMountOptions("redis://localhost:6379/0", tmpdir_);
  VolumeImpl vol;
  Status st = vol.LoadFrom(options);
  EXPECT_FALSE(st.ok());
}

TEST_F(VolumeImplTest, LoadFromMissingFile) {
  auto options = makeMountOptions("memory://local", "nonexistent_vol_impl_test");
  VolumeImpl vol;
  Status st = vol.LoadFrom(options);
  EXPECT_FALSE(st.ok());
}
