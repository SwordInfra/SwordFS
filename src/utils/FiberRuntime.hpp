// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Per-FUSE-worker fiber runtime.  Each FUSE worker thread owns a
// FiberRuntime instance that wraps an EventBase + background driver thread.
// FUSE callbacks submit fiber tasks via RunInFiber() and return immediately;
// the driver thread runs EventBase::loopForever() to advance all fibers.
//
// IMPORTANT: callers must capture all arguments by value — the calling
// stack frame is gone by the time the fiber executes.
//
// Usage from a FUSE callback:
// @code
//   RunInFiber([vfs, req, parent, name = std::string(name)] {
//     vfs->Lookup(req, parent, name.c_str());
//   });
// @endcode

#pragma once

#include <folly/fibers/FiberManagerMap.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "utils/Context.hpp"
#include "utils/Synchronization.hpp"

namespace folly {
class EventBase;
}

namespace swordfs::utils {

class FiberRuntime {
 public:
  FiberRuntime();
  ~FiberRuntime();

  FiberRuntime(const FiberRuntime &) = delete;
  FiberRuntime &operator=(const FiberRuntime &) = delete;

  /// Submit `fn` as a folly fiber. Returns immediately; the fiber runs on
  /// the driver thread.
  template <typename Fn>
  bool Submit(Fn &&fn) {
    return Submit(std::forward<Fn>(fn), [] {});
  }

  /// Submit `fn` and invoke `on_reject` exactly once if the task cannot be
  /// admitted or cannot be installed in the FiberManager.
  template <typename Fn, typename RejectFn>
  bool Submit(Fn &&fn, RejectFn &&on_reject) {
    auto rejection = std::forward<RejectFn>(on_reject);
    if (!TryAdmit()) {
      rejection();
      return false;
    }

    try {
      evb_->runInEventBaseThread([this, fn = std::forward<Fn>(fn), rejection]() mutable {
        try {
          auto &fm = folly::fibers::getFiberManagerT<SwordFsContext>(*evb_);
          fm.addTask([this, fn = std::move(fn)]() mutable {
            struct CompletionGuard {
              FiberRuntime *runtime;
              ~CompletionGuard() {
                runtime->CompleteTask();
              }
            } guard{this};
            fn();
          });
        } catch (...) {
          CompleteTask();
          rejection();
        }
      });
    } catch (...) {
      CompleteTask();
      rejection();
      return false;
    }
    return true;
  }

  /// Stop accepting tasks, drain all admitted fibers, and join the driver.
  /// Safe to call more than once.
  void Shutdown();

  /// Whether new tasks can still be admitted.
  bool IsAccepting() const;

 private:
  friend void ShutdownFiberRuntime();

  bool TryAdmit();
  void BeginShutdown();
  void CompleteTask();

 private:
  static constexpr uint64_t kAcceptingBit = uint64_t{1} << 63;
  static constexpr uint64_t kPendingMask = ~kAcceptingBit;

  std::unique_ptr<folly::EventBase> evb_;
  std::thread driver_thread_;
  std::atomic<uint64_t> task_state_{kAcceptingBit};
  FiberBaton drained_;
  ThreadMutex shutdown_mutex_;
  std::vector<std::shared_ptr<FiberBaton>> shutdown_waiters_;
  bool shutdown_owner_claimed_{false};
  bool shutdown_complete_{false};
};

/// Create the calling thread's FiberRuntime.  Idempotent — safe to call
/// multiple times on the same thread.
bool InitFiberRuntime();

/// Tear down all FiberRuntime instances (called at unmount).
void ShutdownFiberRuntime();

/// Returns the calling thread's FiberRuntime, or nullptr.
FiberRuntime *ThisFiberRuntime();

/// Asynchronously execute `fn` as a folly fiber.
/// Each calling thread automatically gets its own FiberRuntime on first use.
template <typename Fn>
bool RunInFiber(Fn &&fn) {
  if (!InitFiberRuntime()) {
    return false;
  }
  auto *rt = ThisFiberRuntime();
  return rt != nullptr && rt->Submit(std::forward<Fn>(fn));
}

/// Asynchronously execute `fn` and invoke `on_reject` if the task cannot be
/// admitted. The rejection callback may run on the submitting thread or on the
/// runtime driver thread.
template <typename Fn, typename RejectFn>
bool RunInFiber(Fn &&fn, RejectFn &&on_reject) {
  if (!InitFiberRuntime()) {
    std::forward<RejectFn>(on_reject)();
    return false;
  }
  auto *rt = ThisFiberRuntime();
  if (rt == nullptr) {
    std::forward<RejectFn>(on_reject)();
    return false;
  }
  return rt->Submit(std::forward<Fn>(fn), std::forward<RejectFn>(on_reject));
}

}  // namespace swordfs::utils
