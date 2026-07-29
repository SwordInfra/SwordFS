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
#include <folly/futures/Future.h>

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

  /// Run |fn| on a pool thread and block the calling fiber until done.
  template <typename Fn>
  auto Run(Fn &&fn) -> decltype(fn()) {
    using Result = decltype(fn());
    folly::fibers::Baton baton;
    if constexpr (std::is_void_v<Result>) {
      auto fut = folly::via(pool_.get(),
                            [&baton, fn = std::forward<Fn>(fn)]() mutable {
                              fn();
                              baton.post();
                            });
      baton.wait();
      std::move(fut).get();
    } else {
      Result result;
      auto fut = folly::via(pool_.get(),
                            [&baton, &result, fn = std::forward<Fn>(fn)]() mutable {
                              result = fn();
                              baton.post();
                            });
      baton.wait();
      std::move(fut).get();
      return result;
    }
  }

 private:
  std::unique_ptr<folly::CPUThreadPoolExecutor> pool_;
};

}  // namespace swordfs::utils
