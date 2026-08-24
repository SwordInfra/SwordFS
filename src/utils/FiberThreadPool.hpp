// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// FiberThreadPool — thread pool for running blocking work from fibers.
// A fiber that needs to do blocking I/O (or any synchronous blocking call)
// dispatches the work here via Run().  The fiber yields while the work runs
// on a pool thread; it resumes automatically when the work completes.
// Other fibers on the same EventBase stay responsive.
//
// Usage:
// @code
//   FiberThreadPool pool(4);
//   auto result = pool.Run([&] {
//     return some_blocking_call(args);  // runs on a pool thread
//   });
// @endcode

#pragma once

#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/futures/Future.h>

#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace swordfs::utils {

class FiberThreadPool {
 public:
  explicit FiberThreadPool(size_t num_threads);
  ~FiberThreadPool();

  FiberThreadPool(const FiberThreadPool &) = delete;
  FiberThreadPool &operator=(const FiberThreadPool &) = delete;

  /// Run |fn| on a pool thread and suspend the calling fiber until done.
  /// Must be called from a running fiber.
  template <typename Fn>
  auto Run(Fn &&fn) -> decltype(fn()) {
    CHECK(folly::fibers::FiberManager::getFiberManagerUnsafe() != nullptr)
        << "FiberThreadPool::Run() must be called from a fiber";

    using Result = decltype(fn());
    folly::Try<Result> result;
    folly::fibers::Baton baton;

    folly::via(pool_.get(), [&] {
      result = folly::makeTryWith([&] { return fn(); });
      baton.post();
    });

    baton.wait();

    if constexpr (std::is_void_v<Result>) {
      result.throwIfFailed();
      return;
    } else {
      return std::move(result).value();
    }
  }

 private:
  std::unique_ptr<folly::CPUThreadPoolExecutor> pool_;
};

}  // namespace swordfs::utils
