// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#include "utils/FiberRuntime.hpp"

#include <folly/io/async/EventBase.h>
#include <glog/logging.h>

#include <memory>
#include <utility>
#include <vector>

namespace swordfs::utils {

namespace {

ThreadMutex g_mutex;
std::vector<std::shared_ptr<FiberBaton>> g_shutdown_waiters;
std::vector<std::shared_ptr<FiberRuntime>> g_runtimes;

enum class RuntimeState {
  kStopped,
  kRunning,
  kStopping,
};
RuntimeState g_state = RuntimeState::kStopped;

}  // namespace

thread_local std::shared_ptr<FiberRuntime> t_runtime;

FiberRuntime::FiberRuntime() : evb_(std::make_unique<folly::EventBase>()) {
  driver_thread_ = std::thread([this] { evb_->loopForever(); });
}

bool FiberRuntime::TryAdmit() {
  uint64_t state = task_state_.load(std::memory_order_acquire);
  while ((state & kAcceptingBit) != 0) {
    CHECK_LT(state & kPendingMask, kPendingMask);
    if (task_state_.compare_exchange_weak(state, state + 1, std::memory_order_acq_rel, std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void FiberRuntime::CompleteTask() {
  const uint64_t previous = task_state_.fetch_sub(1, std::memory_order_acq_rel);
  const uint64_t previous_pending = previous & kPendingMask;
  CHECK_GT(previous_pending, 0);
  if ((previous & kAcceptingBit) == 0 && previous_pending == 1) {
    drained_.post();
  }
}

bool FiberRuntime::IsAccepting() const {
  return (task_state_.load(std::memory_order_acquire) & kAcceptingBit) != 0;
}

void FiberRuntime::BeginShutdown() {
  const uint64_t previous = task_state_.fetch_and(kPendingMask, std::memory_order_acq_rel);
  if ((previous & kAcceptingBit) != 0 && (previous & kPendingMask) == 0) {
    drained_.post();
  }
}

void FiberRuntime::Shutdown() {
  const bool on_driver_thread = driver_thread_.get_id() == std::this_thread::get_id();
#ifndef NDEBUG
  DCHECK(!on_driver_thread) << "FiberRuntime::Shutdown must run outside the driver fiber";
#endif
  BeginShutdown();
  if (on_driver_thread) {
    return;
  }

  std::shared_ptr<FiberBaton> completion_waiter;
  bool owns_shutdown = false;
  {
    std::lock_guard<ThreadMutex> lock(shutdown_mutex_);
    if (shutdown_complete_) {
      return;
    }
    if (shutdown_owner_claimed_) {
      completion_waiter = std::make_shared<FiberBaton>();
      shutdown_waiters_.push_back(completion_waiter);
    } else {
      shutdown_owner_claimed_ = true;
      owns_shutdown = true;
    }
  }

  if (completion_waiter != nullptr) {
    completion_waiter->wait();
    return;
  }
  CHECK(owns_shutdown);

  drained_.wait();
  evb_->terminateLoopSoon();
  if (driver_thread_.joinable()) {
    driver_thread_.join();
  }

  std::vector<std::shared_ptr<FiberBaton>> shutdown_waiters;
  {
    std::lock_guard<ThreadMutex> lock(shutdown_mutex_);
    shutdown_complete_ = true;
    shutdown_waiters.swap(shutdown_waiters_);
  }
  for (const auto &waiter : shutdown_waiters) {
    waiter->post();
  }
}

FiberRuntime::~FiberRuntime() {
  Shutdown();
}

bool InitFiberRuntime() {
  std::shared_ptr<FiberRuntime> stale_runtime;
  bool stopping = false;
  {
    std::lock_guard<ThreadMutex> lock(g_mutex);
    if (g_state == RuntimeState::kStopping) {
      stale_runtime = std::move(t_runtime);
      stopping = true;
    } else if (t_runtime != nullptr && t_runtime->IsAccepting()) {
      return true;
    } else {
      stale_runtime = std::move(t_runtime);
      if (g_state == RuntimeState::kStopped) {
        g_state = RuntimeState::kRunning;
      }

      t_runtime = std::make_shared<FiberRuntime>();
      g_runtimes.push_back(t_runtime);
    }
  }
  stale_runtime.reset();
  return !stopping;
}

void ShutdownFiberRuntime() {
  std::vector<std::shared_ptr<FiberRuntime>> runtimes;
  std::shared_ptr<FiberBaton> completion_waiter;
  bool owns_shutdown = false;
  {
    std::lock_guard<ThreadMutex> lock(g_mutex);
    if (g_state == RuntimeState::kStopped) {
      t_runtime.reset();
      return;
    }
    if (g_state == RuntimeState::kStopping) {
      completion_waiter = std::make_shared<FiberBaton>();
      g_shutdown_waiters.push_back(completion_waiter);
    } else {
      g_state = RuntimeState::kStopping;
      runtimes.swap(g_runtimes);
      for (const auto &runtime : runtimes) {
        runtime->BeginShutdown();
      }
      owns_shutdown = true;
    }
  }

  if (completion_waiter != nullptr) {
    completion_waiter->wait();
    t_runtime.reset();
    return;
  }
  CHECK(owns_shutdown);

  for (auto &runtime : runtimes) {
    runtime->Shutdown();
  }

  std::vector<std::shared_ptr<FiberBaton>> shutdown_waiters;
  {
    std::lock_guard<ThreadMutex> lock(g_mutex);
    g_state = RuntimeState::kStopped;
    shutdown_waiters.swap(g_shutdown_waiters);
  }
  t_runtime.reset();
  for (const auto &waiter : shutdown_waiters) {
    waiter->post();
  }
}

FiberRuntime *ThisFiberRuntime() {
  return t_runtime.get();
}

}  // namespace swordfs::utils
