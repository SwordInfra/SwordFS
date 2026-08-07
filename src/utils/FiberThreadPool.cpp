// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/FiberThreadPool.hpp"

#include <folly/logging/xlog.h>
#include "utils/Logging.hpp"

namespace swordfs::utils {

FiberThreadPool::FiberThreadPool(size_t num_threads)
    : pool_(std::make_unique<folly::CPUThreadPoolExecutor>(num_threads)) {}

FiberThreadPool::~FiberThreadPool() {
  if (pool_) {
    SWORDFS_LOG_INFO << "FiberThreadPool: joining threads...";
    pool_->join();
    SWORDFS_LOG_INFO << "FiberThreadPool: join complete";
  }
}

}  // namespace swordfs::utils
