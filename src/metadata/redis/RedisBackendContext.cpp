// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "metadata/redis/RedisBackendContext.hpp"

#include <glog/logging.h>

#include "metadata/redis/RedisMetaClient.hpp"
#include "utils/BlockingExecutor.hpp"
#include "utils/ExecutionDomain.hpp"

namespace swordfs::metadata {

RedisBackendContext::RedisBackendContext(const RedisMetaConfig &config, size_t worker_count)
    : client_(std::make_unique<RedisMetaClient>(config)),
      executor_(std::make_unique<utils::BlockingExecutor>(worker_count, "swordfs-redis")) {
  utils::ExpectInThreadDomain();
}

RedisBackendContext::~RedisBackendContext() {
  CHECK(client_ == nullptr && executor_ == nullptr)
      << "RedisBackendContext must be shut down by its thread-domain owner before destruction";
}

RedisMetaClient &RedisBackendContext::client() const {
  CHECK(client_ != nullptr) << "Redis backend is shut down";
  return *client_;
}

utils::BlockingExecutor &RedisBackendContext::executor() const {
  CHECK(executor_ != nullptr) << "Redis backend is shut down";
  return *executor_;
}

void RedisBackendContext::Shutdown() {
  utils::ExpectInThreadDomain();
  if (executor_ != nullptr) {
    executor_->Shutdown();
    executor_.reset();
  }
  client_.reset();
}

}  // namespace swordfs::metadata
