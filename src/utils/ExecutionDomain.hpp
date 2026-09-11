// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/fibers/FiberManagerInternal.h>

namespace swordfs::utils {

enum class ExecutionDomain {
  kUnknown,
  kFiber,
  kBlockingThread,
};

// Explicit marking is needed only for blocking-worker code. Fiber identity is
// derived from Folly's active fiber so it remains correct across fiber yields
// and switches on the same EventBase thread.
inline thread_local bool t_blocking_thread_domain = false;

inline ExecutionDomain CurrentExecutionDomain() {
  if (folly::fibers::onFiber()) {
    return ExecutionDomain::kFiber;
  }
  return t_blocking_thread_domain ? ExecutionDomain::kBlockingThread : ExecutionDomain::kUnknown;
}

class ScopedBlockingThreadDomain {
 public:
  ScopedBlockingThreadDomain() : previous_(t_blocking_thread_domain) {
    t_blocking_thread_domain = true;
  }

  ~ScopedBlockingThreadDomain() {
    t_blocking_thread_domain = previous_;
  }

  ScopedBlockingThreadDomain(const ScopedBlockingThreadDomain &) = delete;
  ScopedBlockingThreadDomain &operator=(const ScopedBlockingThreadDomain &) = delete;

 private:
  bool previous_;
};

}  // namespace swordfs::utils
