// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/FiberRuntime.hpp"

#include <mutex>
#include <vector>

namespace swordfs::utils {

namespace {

std::mutex g_mutex;
std::vector<FiberRuntime*> g_runtimes;
bool g_shutdown = false;

}  // namespace

thread_local FiberRuntime* t_runtime = nullptr;

FiberRuntime::FiberRuntime() : evb_(std::make_unique<folly::EventBase>()) {
  driver_thread_ = std::thread([this] { evb_->loopForever(); });
}

FiberRuntime::~FiberRuntime() {
  evb_->terminateLoopSoon();
  if (driver_thread_.joinable()) {
    driver_thread_.join();
  }
}

void InitFiberRuntime() {
  if (t_runtime != nullptr) return;

  std::lock_guard<std::mutex> lock(g_mutex);
  if (t_runtime != nullptr || g_shutdown) return;

  t_runtime = new FiberRuntime();
  g_runtimes.push_back(t_runtime);
}

void ShutdownFiberRuntime() {
  std::vector<FiberRuntime*> runtimes;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shutdown = true;
    runtimes.swap(g_runtimes);
  }

  for (auto* rt : runtimes) {
    delete rt;
  }
}

FiberRuntime* ThisFiberRuntime() { return t_runtime; }

}  // namespace swordfs::utils
