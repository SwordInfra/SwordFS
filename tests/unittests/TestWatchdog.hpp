// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <event2/event.h>
#include <folly/fibers/Baton.h>
#include <folly/io/async/EventBase.h>

#include <chrono>
#include <thread>
#include <utility>

namespace swordfs::test {

// This is a test-liveness watchdog, not a production timing contract. Five
// seconds leaves scheduler headroom while ensuring a missing completion event
// fails the test instead of consuming the whole CI job timeout.
inline constexpr auto kAsyncCompletionWatchdog = std::chrono::seconds(5);

inline bool WaitForBaton(folly::fibers::Baton &baton,
                         std::chrono::steady_clock::duration timeout = kAsyncCompletionWatchdog) {
  return baton.try_wait_for(timeout);
}

template <typename Predicate>
bool DriveEventBaseUntil(folly::EventBase &evb, Predicate &&predicate,
                         std::chrono::steady_clock::duration timeout = kAsyncCompletionWatchdog) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    // A blocking loopOnce() would defeat the watchdog when the regression is
    // precisely that no fiber/event ever becomes runnable.
    evb.loopOnce(EVLOOP_NONBLOCK);
    std::this_thread::yield();
  }
  return true;
}

}  // namespace swordfs::test
