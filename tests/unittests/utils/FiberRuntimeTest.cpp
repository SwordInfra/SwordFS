// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FiberRuntime lifecycle and admission.

#include <folly/fibers/Baton.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "utils/FiberRuntime.hpp"

using swordfs::utils::FiberRuntime;

namespace {

constexpr auto kStateTransitionWatchdog = std::chrono::seconds(5);

template <typename Predicate>
bool WaitUntil(Predicate &&predicate) {
  const auto deadline = std::chrono::steady_clock::now() + kStateTransitionWatchdog;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

}  // namespace

TEST(FiberRuntimeTest, SubmittedTaskRunsInFiberDomain) {
  FiberRuntime runtime;
  std::atomic<swordfs::utils::ExecutionDomain> domain{swordfs::utils::ExecutionDomain::kThread};

  ASSERT_TRUE(
      runtime.Submit([&] { domain.store(swordfs::utils::CurrentExecutionDomain(), std::memory_order_release); }));
  runtime.Shutdown();

  EXPECT_EQ(domain.load(std::memory_order_acquire), swordfs::utils::ExecutionDomain::kFiber);
}

TEST(FiberRuntimeTest, ShutdownDrainsAdmittedTasks) {
  FiberRuntime runtime;
  std::atomic<bool> ran{false};
  std::atomic<bool> shutdown_returned{false};
  folly::fibers::Baton task_started;
  folly::fibers::Baton release_task;

  ASSERT_TRUE(runtime.Submit([&] {
    task_started.post();
    release_task.wait();
    ran.store(true, std::memory_order_release);
  }));
  task_started.wait();

  std::thread shutdown([&] {
    runtime.Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
  });
  const bool stopped_accepting = WaitUntil([&] { return !runtime.IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "shutdown must stop admission within the test watchdog";

  EXPECT_FALSE(shutdown_returned.load(std::memory_order_acquire));
  release_task.post();
  shutdown.join();
  EXPECT_TRUE(ran.load(std::memory_order_acquire));
  EXPECT_FALSE(runtime.IsAccepting());
}

TEST(FiberRuntimeTest, SubmitIsRejectedAfterShutdown) {
  FiberRuntime runtime;
  runtime.Shutdown();

  EXPECT_FALSE(runtime.Submit([] {}));
}

TEST(FiberRuntimeTest, RejectionCallbackRunsWhenSubmitIsRejected) {
  FiberRuntime runtime;
  runtime.Shutdown();
  std::atomic<int> rejected{0};

  EXPECT_FALSE(runtime.Submit([] {}, [&] { rejected.fetch_add(1, std::memory_order_relaxed); }));
  EXPECT_EQ(rejected.load(std::memory_order_relaxed), 1);
}

TEST(FiberRuntimeTest, SubmitIsRejectedWhileShutdownDrainsAdmittedTask) {
  FiberRuntime runtime;
  folly::fibers::Baton task_started;
  folly::fibers::Baton release_task;
  std::atomic<int> rejected{0};

  ASSERT_TRUE(runtime.Submit([&] {
    task_started.post();
    release_task.wait();
  }));
  task_started.wait();

  std::thread shutdown([&] { runtime.Shutdown(); });
  const bool stopped_accepting = WaitUntil([&] { return !runtime.IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "shutdown must stop admission within the test watchdog";

  EXPECT_FALSE(runtime.Submit([] {}, [&] { rejected.fetch_add(1, std::memory_order_relaxed); }));
  EXPECT_EQ(rejected.load(std::memory_order_relaxed), 1);

  release_task.post();
  shutdown.join();
}

#ifdef NDEBUG
TEST(FiberRuntimeTest, ShutdownFromDriverFiberDefersJoinWithoutDeadlock) {
  FiberRuntime runtime;
  std::promise<void> shutdown_returned;
  auto shutdown_future = shutdown_returned.get_future();
  constexpr auto kShutdownWatchdog = std::chrono::seconds(1);

  ASSERT_TRUE(runtime.Submit([&] {
    runtime.Shutdown();
    shutdown_returned.set_value();
  }));

  // This is a liveness watchdog, not a synchronization delay: Shutdown()
  // invoked by the driver fiber must return without attempting to join itself.
  EXPECT_EQ(shutdown_future.wait_for(kShutdownWatchdog), std::future_status::ready);
  runtime.Shutdown();
  EXPECT_FALSE(runtime.IsAccepting());
}
#endif

TEST(FiberRuntimeTest, RunInFiberRejectsWhileGlobalRuntimeIsStopping) {
  ASSERT_TRUE(swordfs::utils::InitFiberRuntime());
  auto *runtime = swordfs::utils::ThisFiberRuntime();
  ASSERT_NE(runtime, nullptr);

  folly::fibers::Baton task_started;
  folly::fibers::Baton release_task;
  ASSERT_TRUE(runtime->Submit([&] {
    task_started.post();
    release_task.wait();
  }));
  task_started.wait();

  std::thread shutdown([] { swordfs::utils::ShutdownFiberRuntime(); });
  const bool stopped_accepting = WaitUntil([&] { return !runtime->IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "global shutdown must stop admission within the test watchdog";

  std::atomic<int> rejected{0};
  EXPECT_FALSE(swordfs::utils::RunInFiber([] {}, [&] { rejected.fetch_add(1, std::memory_order_relaxed); }));
  EXPECT_EQ(rejected.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(swordfs::utils::ThisFiberRuntime(), nullptr);

  release_task.post();
  shutdown.join();
}

TEST(FiberRuntimeTest, GlobalShutdownRejectsAndDrainsMultipleDriverRuntimes) {
  struct WorkerState {
    folly::fibers::Baton initialized;
    folly::fibers::Baton task_started;
    folly::fibers::Baton release_task;
    folly::fibers::Baton check_rejection;
    folly::fibers::Baton checked_rejection;
    folly::fibers::Baton restart;
    folly::fibers::Baton restarted;
    folly::fibers::Baton finish;
    std::atomic<FiberRuntime *> runtime{nullptr};
    std::atomic<FiberRuntime *> restarted_runtime{nullptr};
    std::atomic<bool> init_ok{false};
    std::atomic<bool> submit_ok{false};
    std::atomic<bool> rejected_during_shutdown{false};
    std::atomic<bool> restart_ok{false};
  } first, second;

  swordfs::utils::ShutdownFiberRuntime();

  auto worker = [](WorkerState *state) {
    state->init_ok.store(swordfs::utils::InitFiberRuntime(), std::memory_order_release);
    auto *runtime = swordfs::utils::ThisFiberRuntime();
    state->runtime.store(runtime, std::memory_order_release);
    const bool submit_ok = runtime != nullptr && runtime->Submit([state] {
      state->task_started.post();
      state->release_task.wait();
    });
    state->submit_ok.store(submit_ok, std::memory_order_release);
    if (!submit_ok) {
      state->task_started.post();
    }
    state->initialized.post();

    state->check_rejection.wait();
    state->rejected_during_shutdown.store(!swordfs::utils::RunInFiber([] {}), std::memory_order_release);
    state->checked_rejection.post();

    state->restart.wait();
    state->restart_ok.store(swordfs::utils::InitFiberRuntime(), std::memory_order_release);
    state->restarted_runtime.store(swordfs::utils::ThisFiberRuntime(), std::memory_order_release);
    state->restarted.post();
    state->finish.wait();
  };

  std::thread first_worker(worker, &first);
  std::thread second_worker(worker, &second);

  first.initialized.wait();
  second.initialized.wait();
  first.task_started.wait();
  second.task_started.wait();
  ASSERT_TRUE(first.init_ok.load(std::memory_order_acquire));
  ASSERT_TRUE(second.init_ok.load(std::memory_order_acquire));
  ASSERT_TRUE(first.submit_ok.load(std::memory_order_acquire));
  ASSERT_TRUE(second.submit_ok.load(std::memory_order_acquire));

  auto *first_runtime = first.runtime.load(std::memory_order_acquire);
  auto *second_runtime = second.runtime.load(std::memory_order_acquire);
  ASSERT_NE(first_runtime, nullptr);
  ASSERT_NE(second_runtime, nullptr);
  ASSERT_NE(first_runtime, second_runtime);

  std::thread shutdown([] { swordfs::utils::ShutdownFiberRuntime(); });
  const bool stopped_accepting =
      WaitUntil([&] { return !first_runtime->IsAccepting() && !second_runtime->IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "global shutdown must stop admission on every runtime within the test watchdog";

  first.check_rejection.post();
  second.check_rejection.post();
  first.checked_rejection.wait();
  second.checked_rejection.wait();
  EXPECT_TRUE(first.rejected_during_shutdown.load(std::memory_order_acquire));
  EXPECT_TRUE(second.rejected_during_shutdown.load(std::memory_order_acquire));

  first.release_task.post();
  second.release_task.post();
  shutdown.join();

  first.restart.post();
  second.restart.post();
  first.restarted.wait();
  second.restarted.wait();
  EXPECT_TRUE(first.restart_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(second.restart_ok.load(std::memory_order_acquire));
  EXPECT_NE(first.restarted_runtime.load(std::memory_order_acquire), nullptr);
  EXPECT_NE(second.restarted_runtime.load(std::memory_order_acquire), nullptr);

  swordfs::utils::ShutdownFiberRuntime();
  first.finish.post();
  second.finish.post();
  first_worker.join();
  second_worker.join();
}

TEST(FiberRuntimeTest, ConcurrentShutdownCallsDrainOnce) {
  FiberRuntime runtime;
  std::atomic<bool> ran{false};
  folly::fibers::Baton task_started;
  folly::fibers::Baton release_task;

  ASSERT_TRUE(runtime.Submit([&] {
    task_started.post();
    release_task.wait();
    ran.store(true, std::memory_order_release);
  }));
  task_started.wait();

  std::thread first([&] { runtime.Shutdown(); });
  std::thread second([&] { runtime.Shutdown(); });
  const bool stopped_accepting = WaitUntil([&] { return !runtime.IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "concurrent shutdown must stop admission within the test watchdog";
  release_task.post();
  first.join();
  second.join();

  EXPECT_TRUE(ran.load(std::memory_order_acquire));
  EXPECT_FALSE(runtime.IsAccepting());
}

TEST(FiberRuntimeTest, GlobalRuntimeCanRestartAfterShutdown) {
  ASSERT_TRUE(swordfs::utils::InitFiberRuntime());
  ASSERT_NE(swordfs::utils::ThisFiberRuntime(), nullptr);

  swordfs::utils::ShutdownFiberRuntime();
  EXPECT_EQ(swordfs::utils::ThisFiberRuntime(), nullptr);

  ASSERT_TRUE(swordfs::utils::InitFiberRuntime());
  ASSERT_NE(swordfs::utils::ThisFiberRuntime(), nullptr);
  swordfs::utils::ShutdownFiberRuntime();
  EXPECT_EQ(swordfs::utils::ThisFiberRuntime(), nullptr);
}
