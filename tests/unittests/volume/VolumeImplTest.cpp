// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "FiberTest.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/mem/MemMetaImpl.hpp"
#include "metadata/mem/VolumeFile.hpp"
#include "storage/IDataEngine.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkOverwriteMechanism;
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
      "--chunk-overwrite-strategy",
      "redis_cache",
  });

  VolumeImpl volume;
  const auto status = volume.CreateFrom(swordfs::config::ConfigCenter::Instance());
  EXPECT_EQ(status.ToErrno(), ENOSYS);
  EXPECT_EQ(volume.config().name, "adaptervol");
  EXPECT_EQ(volume.config().bucket, "swordfs-test-data://endpoint/bucket");
  EXPECT_EQ(volume.config().region, "adapter-region");
  EXPECT_EQ(volume.config().chunk_size, 4096U);
  EXPECT_EQ(volume.config().chunk_overwrite_mechanism, ChunkOverwriteMechanism::kRedisCache);
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

TEST(VolumeImplConfigAdapterTest, CreateFromRejectsUnknownMechanismName) {
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
      "--chunk-overwrite-strategy",
      "unknown-mechanism",
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
                                  ChunkOverwriteMechanism mechanism = ChunkOverwriteMechanism::kWholeObject) const {
    return FormatOptions{
        .name = makeVolumeName(vol_name),
        .meta_url = meta_url,
        .bucket = bucket_url,
        .region = storage_region,
        .chunk_overwrite_mechanism = mechanism,
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
  EXPECT_EQ(vol.config().chunk_overwrite_mechanism, ChunkOverwriteMechanism::kWholeObject);
  EXPECT_EQ(vol.config().chunk_index_format_version, 1U);
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

TEST_F(VolumeImplTest, FormatRejectsUnimplementedStrategy) {
  auto options = makeFormatOptions("memory://local", "testvol", "s3://endpoint.example.com/bucket", "",
                                   ChunkOverwriteMechanism::kRedisCache);
  VolumeImpl vol;
  const auto status = vol.CreateFrom(options);
  EXPECT_TRUE(status.ToErrno() == ENOSYS) << status.message();
  EXPECT_FALSE(VolumeFile{options.name}.Exists());
}

TEST_F(VolumeImplTest, MountUsesPersistedStrategyAndRejectsUnsupportedVersion) {
  auto format_options = makeFormatOptions("memory://local");
  VolumeImpl formatted;
  const auto format_status = formatted.CreateFrom(format_options);
  ASSERT_TRUE(format_status.ok()) << format_status.message();

  VolumeImpl mounted;
  auto mount_options = makeMountOptions("memory://local");
  ASSERT_TRUE(mounted.LoadFrom(mount_options).ok());
  ASSERT_NE(mounted.chunk_overwrite_strategy(), nullptr);
  EXPECT_EQ(mounted.config().chunk_overwrite_mechanism, ChunkOverwriteMechanism::kWholeObject);

  SwordFsVolume stored = mounted.config();
  stored.chunk_index_format_version = 2;
  const auto unsupported_options = makeMountOptions("memory://local");
  stored.name = unsupported_options.name;
  ASSERT_TRUE(VolumeFile{stored.name}.Write(stored).ok());
  VolumeImpl unsupported;
  EXPECT_TRUE(unsupported.LoadFrom(unsupported_options).ToErrno() == ENOSYS);
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
  EXPECT_NE(VolumeImpl::Instance().data_engine(), nullptr);
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
  ASSERT_NE(VolumeImpl::Instance().data_engine(), nullptr);
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
