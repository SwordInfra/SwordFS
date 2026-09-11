// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// BlockingExecutor — executes synchronous/blocking work on POSIX worker
// threads while preserving an explicit execution-domain contract at the call
// site. Callers must choose RunFromFiber() or RunFromThread(); the executor
// never guesses the caller domain.

#pragma once

#include <folly/ScopeGuard.h>
#include <folly/Try.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/fibers/Baton.h>
#include <folly/futures/Future.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "utils/ExecutionDomain.hpp"

namespace swordfs::utils {

class BlockingExecutor {
 public:
  explicit BlockingExecutor(size_t num_threads, const std::string &thread_name = "swordfs-blocking");
  ~BlockingExecutor();

  // Explicit lifecycle hook for owners that may share this executor with
  // fiber-domain objects. Shutdown itself is thread-domain control work and
  // drains all workers before releasing the underlying pool.
  void Shutdown();

  BlockingExecutor(const BlockingExecutor &) = delete;
  BlockingExecutor &operator=(const BlockingExecutor &) = delete;

  // Offload blocking work from a Folly fiber. The calling fiber suspends on a
  // Baton while the callback runs on a POSIX worker thread.
  template <typename Fn>
  auto RunFromFiber(Fn &&fn) -> decltype(fn()) {
    ExpectInFiberDomain();
    CHECK(pool_ != nullptr) << "BlockingExecutor is shut down";

    using Result = decltype(fn());
    folly::Try<Result> result;
    folly::fibers::Baton baton;

    folly::via(pool_.get(), [&] {
      auto post_baton = folly::makeGuard([&] { baton.post(); });
      ExpectInThreadDomain();
      result = folly::makeTryWith([&] { return fn(); });
    });

    baton.wait();

    if constexpr (std::is_void_v<Result>) {
      result.throwIfFailed();
      return;
    } else {
      return std::move(result).value();
    }
  }

  // Offload blocking work from a normal POSIX thread. The caller blocks on the
  // Future while the callback runs on a worker thread.
  template <typename Fn>
  auto RunFromThread(Fn &&fn) -> decltype(fn()) {
    ExpectInThreadDomain();
    CHECK(pool_ != nullptr) << "BlockingExecutor is shut down";

    auto future = folly::via(pool_.get(), [fn = std::forward<Fn>(fn)]() mutable {
      ExpectInThreadDomain();
      return fn();
    });
    if constexpr (std::is_void_v<decltype(fn())>) {
      std::move(future).get();
      return;
    } else {
      return std::move(future).get();
    }
  }

 private:
  std::unique_ptr<folly::CPUThreadPoolExecutor> pool_;
};

}  // namespace swordfs::utils
