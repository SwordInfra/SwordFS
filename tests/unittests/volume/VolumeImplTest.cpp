// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "FiberTest.hpp"
#include "UnsupportedMetaEngine.hpp"
#include "VolumeRuntimeTestUtils.hpp"
#include "config/ConfigCenter.hpp"
#include "metadata/MetaEngineRegistry.hpp"
#include "metadata/redis/RedisTestUtils.hpp"
#include "storage/IDataEngine.hpp"
#include "volume/VolumeImpl.hpp"

using swordfs::metadata::ChunkType;
using swordfs::metadata::SwordFsVolume;
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

class InvalidChunkMetadataEngine final : public swordfs::test::UnsupportedMetaEngine {
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

  Status BindChunkMetadataBridge(swordfs::chunk::internal::ChunkMetadataBridge *) override {
    return Status::OK();
  }

 private:
  Result result_;
};

// Keep mount argument / error-path tests offline; an injected LoadVolume is
// not a substitute for the Redis persistence tests below.
MountOptions PrepareInjectedMount(
    SwordFsVolume config,
    std::unique_ptr<swordfs::metadata::IMetaEngine> engine =
        std::make_unique<swordfs::test::ConfiguredMetaEngine<swordfs::test::UnsupportedMetaEngine>>()) {
  swordfs::test::RegisterTestVolumeEngines();
  swordfs::test::pending_volume = std::move(config);
  swordfs::test::pending_meta_engine = std::move(engine);
  swordfs::test::pending_data_engine.reset();
  return swordfs::test::MakeTestMountOptions(swordfs::test::pending_volume.name);
}

// Models decoding an invalid volume record on LoadVolume without implementing
// a second persistent metadata backend; the canonical decoder is production.
class EncodedVolumeMetaEngine final : public swordfs::test::UnsupportedMetaEngine {
 public:
  explicit EncodedVolumeMetaEngine(std::string encoded) : encoded_(std::move(encoded)) {
  }

  Status LoadVolume(SwordFsVolume *out) override {
    return out->ParseFrom(encoded_);
  }

 private:
  std::string encoded_;
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
      std::make_unique<swordfs::test::ConfiguredMetaEngine<swordfs::test::UnsupportedMetaEngine>>();
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
    run_token_ = swordfs::test::UniqueRedisTestNamespace("volumeimpl");
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
    return sanitize(vol_name) + sanitize(test_name) + sanitize(run_token_);
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

  std::string run_token_;
};

// Only the real lifecycle tests require a Redis service; parser/error-path
// tests remain executable without one.
class VolumeImplRedisTest : public VolumeImplTest {
 protected:
  void SetUp() override {
    VolumeImplTest::SetUp();
    const char *url = std::getenv("SWORDFS_REDIS_TEST_URL");
    if (url == nullptr || url[0] == '\0') {
      GTEST_SKIP() << "SWORDFS_REDIS_TEST_URL is not configured";
    }
  }

  std::string RedisUrl() const {
    return std::getenv("SWORDFS_REDIS_TEST_URL");
  }
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
  swordfs::test::RegisterTestVolumeEngines();
  swordfs::test::pending_meta_engine =
      std::make_unique<swordfs::test::ConfiguredMetaEngine<swordfs::test::UnsupportedMetaEngine>>();
  auto options = makeFormatOptions("swordfs-test-meta://local");
  VolumeImpl volume;
  const auto status = volume.CreateFrom(options);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(volume.config().name, options.name);
  EXPECT_EQ(volume.config().chunk_type, ChunkType::kCow);
}

TEST_F(VolumeImplTest, CreateFromRejectsInvalidBucketUrl) {
  auto options = makeFormatOptions("swordfs-test-meta://local", "testvol", "not-a-storage-url");
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

TEST_F(VolumeImplRedisTest, FormatRejectsUnimplementedChunkType) {
  auto options =
      makeFormatOptions(RedisUrl(), "unimplemented", "s3://endpoint.example.com/bucket", "", ChunkType::kRedisCache);
  VolumeImpl volume;
  const auto status = volume.CreateFrom(options);
  EXPECT_EQ(status.ToErrno(), ENOSYS) << status.message();

  // Replacing VolumeFile with Redis must not weaken the former assertion:
  // an unsuccessful Format must leave no authoritative volume record.
  VolumeImpl reader;
  const auto load_status = reader.LoadFrom(makeMountOptions(RedisUrl(), "unimplemented"));
  EXPECT_EQ(load_status.ToErrno(), ENOENT) << load_status.message();
}

TEST_F(VolumeImplRedisTest, MountUsesPersistedChunkTypeAndRejectsUnimplementedType) {
  auto format_options = makeFormatOptions(RedisUrl());
  VolumeImpl formatted;
  auto status = formatted.CreateFrom(format_options);
  ASSERT_TRUE(status.ok()) << status.message();

  VolumeImpl mounted;
  status = mounted.LoadFrom(makeMountOptions(RedisUrl()));
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_NE(mounted.chunk_factory(), nullptr);
  EXPECT_EQ(mounted.config().chunk_type, ChunkType::kCow);
  EXPECT_EQ(mounted.config().name, format_options.name);

  // Check unsupported persisted chunk selection in isolation: the real
  // Redis round trip above proves the canonical persisted COW selection.
  SwordFsVolume unsupported = mounted.config();
  unsupported.chunk_type = ChunkType::kRedisCache;
  const auto mount_options = PrepareInjectedMount(unsupported);
  VolumeImpl rejected;
  status = rejected.LoadFrom(mount_options);
  EXPECT_EQ(status.ToErrno(), ENOSYS) << status.message();
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

TEST_F(VolumeImplTest, CreateFromNormalizesDataEngineIdentity) {
  swordfs::test::RegisterTestVolumeEngines();
  swordfs::test::pending_meta_engine =
      std::make_unique<swordfs::test::ConfiguredMetaEngine<swordfs::test::UnsupportedMetaEngine>>();
  auto options = makeFormatOptions("swordfs-test-meta://local", "testvol", "S3://endpoint.example.com/bucket");
  VolumeImpl volume;
  ASSERT_TRUE(volume.CreateFrom(options).ok());
  EXPECT_EQ(volume.config().storage, "s3");
}

TEST_F(VolumeImplRedisTest, CreateFromRedisEngine) {
  auto format_options = makeFormatOptions(RedisUrl(), "redis", "s3://endpoint.example.com/bucket", "us-east-1");

  VolumeImpl formatted;
  auto status = formatted.CreateFrom(format_options);
  ASSERT_TRUE(status.ok()) << status.message();

  VolumeImpl mounted;
  status = mounted.LoadFrom(makeMountOptions(RedisUrl(), "redis"));
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(mounted.config().name, format_options.name);
  EXPECT_EQ(mounted.config().storage, "s3");
  EXPECT_EQ(mounted.config().bucket, format_options.bucket);
  EXPECT_EQ(mounted.config().region, "us-east-1");
  EXPECT_EQ(mounted.config().chunk_type, ChunkType::kCow);

  VolumeImpl duplicate;
  status = duplicate.CreateFrom(format_options);
  EXPECT_EQ(status.ToErrno(), EEXIST) << status.message();
}

TEST_F(VolumeImplRedisTest, LoadFromS3Engine) {
  auto format_options = makeFormatOptions(RedisUrl(), "s3", "s3://myhost.example.com/mybucket", "us-west-2");
  VolumeImpl formatted;
  ASSERT_TRUE(formatted.CreateFrom(format_options).ok());

  VolumeImpl loaded;
  const auto status = loaded.LoadFrom(makeMountOptions(RedisUrl(), "s3"));
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.config().bucket, format_options.bucket);
  EXPECT_EQ(loaded.config().region, "us-west-2");
  loaded.Shutdown();
}

TEST_F(VolumeImplTest, LoadFromUnknownDataEngine) {
  SwordFsVolume stored;
  stored.name = makeVolumeName("unknown-data-engine");
  stored.storage = "does-not-exist";
  stored.bucket = "s3://endpoint.example.com/bucket";
  const auto options = PrepareInjectedMount(stored);

  VolumeImpl volume;
  const auto status = volume.LoadFrom(options);
  EXPECT_EQ(status.ToErrno(), ENOSYS) << status.message();
}

TEST_F(VolumeImplTest, LoadFromUsesPersistedDataEngineIdentity) {
  // The configured identity, not the bucket URL scheme, selects the engine.
  SwordFsVolume stored;
  stored.name = makeVolumeName("stored-engine-identity");
  stored.storage = "does-not-exist";
  stored.bucket = "s3://endpoint.example.com/bucket";
  const auto options = PrepareInjectedMount(stored);

  VolumeImpl volume;
  const auto status = volume.LoadFrom(options);
  EXPECT_EQ(status.ToErrno(), ENOSYS) << status.message();
  EXPECT_EQ(volume.config().storage, "does-not-exist");
}

TEST_F(VolumeImplTest, LoadFromRejectsMissingDataEngineIdentity) {
  SwordFsVolume stored;
  stored.name = makeVolumeName("missing-storage-identity");
  stored.bucket = "s3://endpoint.example.com/bucket";
  const auto options = PrepareInjectedMount(stored, std::make_unique<EncodedVolumeMetaEngine>(stored.SerializeTo()));

  VolumeImpl volume;
  const auto status = volume.LoadFrom(options);
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
}

TEST_F(VolumeImplTest, LoadFromRejectsMissingDataEngineLocation) {
  SwordFsVolume stored;
  stored.name = makeVolumeName("missing-storage-location");
  stored.storage = "s3";
  const auto options = PrepareInjectedMount(stored, std::make_unique<EncodedVolumeMetaEngine>(stored.SerializeTo()));

  VolumeImpl volume;
  const auto status = volume.LoadFrom(options);
  EXPECT_EQ(status.ToErrno(), EIO) << status.message();
}

TEST_F(VolumeImplTest, LoadFromS3UrlMissingBucketName) {
  SwordFsVolume stored;
  stored.name = makeVolumeName("missing-bucket-name");
  stored.storage = "s3";
  stored.bucket = "s3://endpoint.example.com";
  const auto options = PrepareInjectedMount(stored);

  VolumeImpl volume;
  const auto status = volume.LoadFrom(options);
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.message().find("missing bucket name"), std::string::npos) << status.message();
}

TEST_F(VolumeImplRedisTest, CreateFromVolumeAlreadyExists) {
  auto options = makeFormatOptions(RedisUrl());
  VolumeImpl first;
  ASSERT_TRUE(first.CreateFrom(options).ok());

  VolumeImpl second;
  const auto status = second.CreateFrom(options);
  EXPECT_EQ(status.ToErrno(), EEXIST) << status.message();
}

// ── LoadFrom ────────────────────────────────────────────────────────

TEST_F(VolumeImplRedisTest, LoadFromSucceeds) {
  auto options = makeFormatOptions(RedisUrl());
  VolumeImpl formatted;
  ASSERT_TRUE(formatted.CreateFrom(options).ok());

  VolumeImpl loaded;
  const auto status = loaded.LoadFrom(makeMountOptions(RedisUrl()));
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(loaded.config().name, options.name);
  EXPECT_EQ(loaded.config().chunk_type, ChunkType::kCow);
  EXPECT_NE(loaded.chunk_factory(), nullptr);
}

TEST_F(VolumeImplTest, LoadFromUnsupportedEngine) {
  VolumeImpl volume;
  const auto status = volume.LoadFrom(makeMountOptions("unregistered://localhost"));
  EXPECT_EQ(status.ToErrno(), ENOSYS) << status.message();
}

TEST_F(VolumeImplTest, RemovedMemoryEngineCannotFormatOrMount) {
  auto &registry = swordfs::metadata::MetaEngineRegistry::Instance();
  EXPECT_FALSE(registry.Available("memory"));

  std::unique_ptr<swordfs::metadata::IMetaEngine> engine;
  const auto registry_status = registry.CreateInstance("memory", "memory://local", makeVolumeName("retired"), &engine);
  EXPECT_EQ(registry_status.ToErrno(), ENOSYS) << registry_status.message();
  EXPECT_EQ(engine, nullptr);

  auto options = makeFormatOptions("memory://local", "retired");
  VolumeImpl formatter;
  const auto format_status = formatter.CreateFrom(options);
  EXPECT_EQ(format_status.ToErrno(), ENOSYS) << format_status.message();
  EXPECT_EQ(formatter.meta_engine(), nullptr);

  VolumeImpl mounted;
  const auto load_status = mounted.LoadFrom(makeMountOptions("memory://local", "retired"));
  EXPECT_EQ(load_status.ToErrno(), ENOSYS) << load_status.message();
  EXPECT_EQ(mounted.meta_engine(), nullptr);
}

TEST_F(VolumeImplRedisTest, LoadFromMissingFile) {
  VolumeImpl volume;
  const auto status = volume.LoadFrom(makeMountOptions(RedisUrl(), "never-formatted"));
  EXPECT_EQ(status.ToErrno(), ENOENT) << status.message();
}
