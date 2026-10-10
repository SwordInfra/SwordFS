// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <cstdlib>
#include <memory>
#include <string_view>

#include "VolumeRuntimeTestUtils.hpp"
#include "metadata/redis/RedisMetaImpl.hpp"
#include "metadata/redis/RedisTestUtils.hpp"

namespace swordfs::test {

// Format an isolated Redis volume before handing it to VolumeImpl's test
// registry. Namespace/lifecycle tests use real metadata authority rather than
// adding a second in-process filesystem solely for test construction.
inline utils::Status MakeFormattedRedisMetaEngine(std::string_view prefix, metadata::SwordFsVolume *config,
                                                  std::unique_ptr<metadata::RedisMetaImpl> *out) {
  if (config == nullptr || out == nullptr) {
    return utils::Status::InvalidArgument("Redis test volume output is null");
  }
  const char *url = std::getenv("SWORDFS_REDIS_TEST_URL");
  if (url == nullptr) {
    return utils::Status::NotSupported("SWORDFS_REDIS_TEST_URL is not configured");
  }
  metadata::RedisMetaConfig redis_config;
  auto status = metadata::ParseRedisMetaUrl(url, &redis_config);
  if (!status.ok()) {
    return status;
  }
  config->name = UniqueRedisTestNamespace(prefix);
  // Persist the test data engine as part of the actual volume format. The
  // volume runtime loads that persistent record rather than the pending
  // configuration used by ConfiguredMetaEngine test doubles.
  config->storage = kTestDataEngine;
  // Persistent volume decoding requires the data engine and its location to
  // be present together. The test engine factory consumes this as an opaque
  // location; there is no external bucket or object-storage dependency.
  config->bucket = std::string(kTestDataEngine) + "://isolated";
  config->chunk_size = 4096;
  auto engine = std::make_unique<metadata::RedisMetaImpl>(redis_config, config->name);
  status = engine->Initialize();
  if (!status.ok()) {
    return status;
  }
  status = engine->FormatVolume(*config);
  if (!status.ok()) {
    return status;
  }
  *out = std::move(engine);
  return utils::Status::OK();
}

}  // namespace swordfs::test
