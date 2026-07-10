// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Per-thread fiber runtime — each FUSE worker thread gets its own EventBase.
// Use RunInFiber() to synchronously execute a callable as a folly fiber; the
// calling thread blocks until the fiber completes.

#pragma once

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include <functional>

namespace swordfs::utils {

/// Initialise the fiber runtime for the calling thread.  Must be called
/// exactly once per FUSE worker thread before any RunInFiber().
void InitFiberRuntime();

/// Tear down the fiber runtime for the calling thread.
void ShutdownFiberRuntime();

/// Returns the thread-local EventBase (nullptr before InitFiberRuntime).
folly::EventBase* ThreadEventBase();

/// Synchronously execute `fn` as a folly fiber on the calling thread's
/// EventBase.  Blocks the caller until the fiber completes.
///
/// Usage from a FUSE callback:
/// @code
///   RunInFiber([&] { vfs_->Lookup(req, parent, name); });
/// @endcode
template <typename Fn>
void RunInFiber(Fn&& fn) {
  auto* evb = ThreadEventBase();
  if (evb == nullptr) {
    // EventBase not initialised — execute directly (graceful fallback).
    fn();
    return;
  }

  folly::fibers::Baton done;
  auto& fm = folly::fibers::getFiberManager(*evb);
  fm.addTask([&] {
    fn();
    done.post();
  });

  // Drive the EventBase until our fiber posts the Baton.
  while (!done.try_wait()) {
    evb->loopOnce();
  }
}

}  // namespace swordfs::utils
