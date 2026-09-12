// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/fibers/FiberManagerInternal.h>
#include <glog/logging.h>

#include <source_location>
#include <string_view>

namespace swordfs::utils {

enum class ExecutionDomain {
  kFiber,
  kThread,
};

// SwordFS intentionally has only two synchronization domains. Code actively
// running as a Folly fiber belongs to kFiber; every other execution context is
// a normal POSIX-thread context and belongs to kThread. Tests for fiber-only
// code must therefore execute inside a real FiberManager rather than relying on
// an unclassified compatibility path.
inline ExecutionDomain CurrentExecutionDomain() {
  return folly::fibers::onFiber() ? ExecutionDomain::kFiber : ExecutionDomain::kThread;
}

inline std::string_view ExecutionDomainName(ExecutionDomain domain) {
  switch (domain) {
    case ExecutionDomain::kFiber:
      return "fiber";
    case ExecutionDomain::kThread:
      return "POSIX-thread";
  }
  return "invalid";
}

// Execution-domain ownership is a component/API contract, not only a locking
// rule. Capture the caller automatically so violations identify the exact
// semantic boundary without requiring hand-written reason strings.
inline void CheckExecutionDomain(ExecutionDomain expected,
                                 std::source_location location = std::source_location::current()) {
#ifndef NDEBUG
  const auto actual = CurrentExecutionDomain();
  DCHECK(actual == expected) << "execution-domain violation at " << location.file_name() << ':' << location.line()
                             << " in " << location.function_name() << ": expected=" << ExecutionDomainName(expected)
                             << ", actual=" << ExecutionDomainName(actual);
#else
  (void)expected;
  (void)location;
#endif
}

inline void ExpectInFiberDomain(std::source_location location = std::source_location::current()) {
  CheckExecutionDomain(ExecutionDomain::kFiber, location);
}

inline void ExpectInThreadDomain(std::source_location location = std::source_location::current()) {
  CheckExecutionDomain(ExecutionDomain::kThread, location);
}

}  // namespace swordfs::utils
