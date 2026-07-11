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
#include <folly/io/async/EventBase.h>

#include <functional>
#include <memory>
#include <thread>

namespace swordfs::utils {

class FiberRuntime {
 public:
  FiberRuntime();
  ~FiberRuntime();

  FiberRuntime(const FiberRuntime&) = delete;
  FiberRuntime& operator=(const FiberRuntime&) = delete;

  /// Submit `fn` as a folly fiber.  Returns immediately; the fiber runs on
  /// the driver thread.
  template <typename Fn>
  void Submit(Fn&& fn) {
    evb_->runInEventBaseThread([this, fn = std::forward<Fn>(fn)]() mutable {
      auto& fm = folly::fibers::getFiberManager(*evb_);
      fm.addTask(std::move(fn));
    });
  }

 private:
  std::unique_ptr<folly::EventBase> evb_;
  std::thread driver_thread_;
};

/// Create the calling thread's FiberRuntime.  Idempotent — safe to call
/// multiple times on the same thread.
void InitFiberRuntime();

/// Tear down all FiberRuntime instances (called at unmount).
void ShutdownFiberRuntime();

/// Returns the calling thread's FiberRuntime, or nullptr.
FiberRuntime* ThisFiberRuntime();

/// Asynchronously execute `fn` as a folly fiber.  Falls back to direct
/// execution if the fiber runtime hasn't been initialised.
template <typename Fn>
void RunInFiber(Fn&& fn) {
  auto* rt = ThisFiberRuntime();
  if (rt == nullptr) {
    // EventBase not initialised — execute directly (graceful fallback).
    fn();
    return;
  }
  rt->Submit(std::forward<Fn>(fn));
}

}  // namespace swordfs::utils
