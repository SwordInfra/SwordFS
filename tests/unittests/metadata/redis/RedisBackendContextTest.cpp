// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include <gtest/gtest.h>

#include "metadata/redis/RedisBackendContext.hpp"
#include "metadata/redis/RedisMetaConfig.hpp"

namespace swordfs::metadata {

TEST(RedisBackendContextTest, RequiresOwnerShutdownBeforeDestruction) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  EXPECT_DEATH(
      {
        RedisBackendContext backend(config, 1);
        (void)backend.client();
      },
      "must be shut down by its thread-domain owner");
}

TEST(RedisBackendContextTest, ShutdownIsIdempotent) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  RedisBackendContext backend(config, 1);
  EXPECT_NE(&backend.client(), nullptr);
  EXPECT_NE(&backend.executor(), nullptr);
  backend.Shutdown();
  backend.Shutdown();
}

TEST(RedisBackendContextTest, RejectsAccessAfterShutdown) {
  RedisMetaConfig config;
  config.host = "127.0.0.1";
  config.retry_attempts = 1;

  EXPECT_DEATH(
      {
        RedisBackendContext backend(config, 1);
        backend.Shutdown();
        (void)backend.client();
      },
      "Redis backend is shut down");

  EXPECT_DEATH(
      {
        RedisBackendContext backend(config, 1);
        backend.Shutdown();
        (void)backend.executor();
      },
      "Redis backend is shut down");
}
}  // namespace swordfs::metadata
