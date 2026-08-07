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
    std::exception_ptr ex;
    if constexpr (std::is_void_v<Result>) {
      auto fut = folly::via(pool_.get(),
                            [&baton, &ex, fn = std::forward<Fn>(fn)]() mutable {
                              try {
                                fn();
                              } catch (...) {
                                ex = std::current_exception();
                              }
                              baton.post();
                            });
      baton.wait();
      std::move(fut).get();
      if (ex) std::rethrow_exception(ex);
    } else {
      Result result;
      auto fut = folly::via(pool_.get(),
                            [&baton, &ex, &result, fn = std::forward<Fn>(fn)]() mutable {
                              try {
                                result = fn();
                              } catch (...) {
                                ex = std::current_exception();
                              }
                              baton.post();
                            });
      baton.wait();
      std::move(fut).get();
      if (ex) std::rethrow_exception(ex);
      return result;
    }
  }

 private:
  std::unique_ptr<folly::CPUThreadPoolExecutor> pool_;
};

}  // namespace swordfs::utils
