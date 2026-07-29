// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/FiberThreadPool.hpp"

namespace swordfs::utils {

FiberThreadPool::FiberThreadPool(size_t num_threads)
    : pool_(std::make_unique<folly::CPUThreadPoolExecutor>(num_threads)) {}

FiberThreadPool::~FiberThreadPool() {
  if (pool_) {
    pool_->join();
  }
}

}  // namespace swordfs::utils
