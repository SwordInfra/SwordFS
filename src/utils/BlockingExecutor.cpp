// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/BlockingExecutor.hpp"

#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/logging/xlog.h>

#include "utils/Logging.hpp"

namespace swordfs::utils {

BlockingExecutor::BlockingExecutor(size_t num_threads, const std::string &thread_name) {
  ExpectInThreadDomain();
  CHECK_GT(num_threads, 0) << "BlockingExecutor requires at least one worker";
  pool_ = std::make_unique<folly::CPUThreadPoolExecutor>(num_threads,
                                                         std::make_shared<folly::NamedThreadFactory>(thread_name));
}

BlockingExecutor::~BlockingExecutor() {
  if (pool_) {
    Shutdown();
  }
}

void BlockingExecutor::Shutdown() {
  ExpectInThreadDomain();
  if (!pool_) {
    return;
  }
  SWORDFS_LOG_INFO << "BlockingExecutor: joining threads...";
  pool_->join();
  pool_.reset();
  SWORDFS_LOG_INFO << "BlockingExecutor: join complete";
}

}  // namespace swordfs::utils
