// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/FiberRuntime.hpp"

namespace swordfs::utils {

namespace {
thread_local folly::EventBase* t_evb = nullptr;
}  // namespace

void InitFiberRuntime() {
  if (t_evb != nullptr) {
    return;
  }
  t_evb = new folly::EventBase();
}

void ShutdownFiberRuntime() {
  delete t_evb;
  t_evb = nullptr;
}

folly::EventBase* ThreadEventBase() { return t_evb; }

}  // namespace swordfs::utils
