// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <event2/event.h>
#include <folly/fibers/Baton.h>
#include <folly/io/async/EventBase.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <thread>
#include <utility>

namespace swordfs::test {

// This is a test-liveness watchdog, not a production timing contract. Five
// seconds leaves scheduler headroom while ensuring a missing completion event
// fails the test instead of consuming the whole CI job timeout.
inline constexpr auto kAsyncCompletionWatchdog = std::chrono::seconds(5);

// RunInTestFiber owns the whole test-body lifetime. A fiber test may need one
// local watchdog interval to observe a missing milestone and another interval
// to release test-owned blockers and drain safely, so its fail-stop bound must
// be comfortably larger than a single observer deadline.
inline constexpr auto kTestFiberCompletionWatchdog = std::chrono::seconds(30);

inline bool WaitForBaton(folly::fibers::Baton &baton,
                         std::chrono::steady_clock::duration timeout = kAsyncCompletionWatchdog) {
  return baton.try_wait_for(timeout);
}

[[noreturn]] inline void AbortOnWatchdogTimeout(std::string_view context) {
  std::fprintf(stderr, "SwordFS test watchdog expired: %.*s\n", static_cast<int>(context.size()), context.data());
  std::fflush(stderr);
  std::abort();
}

inline void WaitForBatonOrAbort(folly::fibers::Baton &baton, std::string_view context,
                                std::chrono::steady_clock::duration timeout = kAsyncCompletionWatchdog) {
  if (!WaitForBaton(baton, timeout)) {
    AbortOnWatchdogTimeout(context);
  }
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

template <typename Predicate>
void DriveEventBaseUntilOrAbort(folly::EventBase &evb, Predicate &&predicate, std::string_view context,
                                std::chrono::steady_clock::duration timeout = kAsyncCompletionWatchdog) {
  if (!DriveEventBaseUntil(evb, std::forward<Predicate>(predicate), timeout)) {
    AbortOnWatchdogTimeout(context);
  }
}

}  // namespace swordfs::test
