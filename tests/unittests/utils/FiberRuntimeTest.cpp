// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

// Unit tests for FiberRuntime lifecycle and admission.

#include <folly/fibers/Baton.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <thread>

#include "TestWatchdog.hpp"
#include "utils/FiberRuntime.hpp"

using swordfs::utils::FiberRuntime;

namespace {

constexpr auto kStateTransitionWatchdog = swordfs::test::kAsyncCompletionWatchdog;

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
  folly::fibers::Baton shutdown_done;

  ASSERT_TRUE(runtime.Submit([&] {
    task_started.post();
    release_task.wait();
    ran.store(true, std::memory_order_release);
  }));
  const bool task_started_in_time = swordfs::test::WaitForBaton(task_started);
  EXPECT_TRUE(task_started_in_time) << "admitted task must start within the test watchdog";
  if (!task_started_in_time) {
    // Release first so cleanup cannot strand a late-starting task on the same
    // missing observation this assertion is reporting.
    release_task.post();
  }

  std::thread shutdown([&] {
    runtime.Shutdown();
    shutdown_returned.store(true, std::memory_order_release);
    shutdown_done.post();
  });
  const bool stopped_accepting = WaitUntil([&] { return !runtime.IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "shutdown must stop admission within the test watchdog";

  EXPECT_FALSE(shutdown_returned.load(std::memory_order_acquire));
  release_task.post();
  swordfs::test::WaitForBatonOrAbort(shutdown_done, "FiberRuntime shutdown completion after task release");
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
  folly::fibers::Baton shutdown_done;
  std::atomic<int> rejected{0};

  ASSERT_TRUE(runtime.Submit([&] {
    task_started.post();
    release_task.wait();
  }));
  const bool task_started_in_time = swordfs::test::WaitForBaton(task_started);
  EXPECT_TRUE(task_started_in_time) << "admitted task must start within the test watchdog";
  if (!task_started_in_time) {
    release_task.post();
  }

  std::thread shutdown([&] {
    runtime.Shutdown();
    shutdown_done.post();
  });
  const bool stopped_accepting = WaitUntil([&] { return !runtime.IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "shutdown must stop admission within the test watchdog";

  EXPECT_FALSE(runtime.Submit([] {}, [&] { rejected.fetch_add(1, std::memory_order_relaxed); }));
  EXPECT_EQ(rejected.load(std::memory_order_relaxed), 1);

  release_task.post();
  swordfs::test::WaitForBatonOrAbort(shutdown_done, "FiberRuntime rejection-test shutdown completion");
  shutdown.join();
}

#ifdef NDEBUG
TEST(FiberRuntimeTest, ShutdownFromDriverFiberDefersJoinWithoutDeadlock) {
  // Keep failure cleanup outside the runtime under test. If the driver fiber
  // deadlocks in Shutdown(), destroying or shutting down that same runtime in
  // the parent test path can repeat the deadlock and hide the actual failure.
  EXPECT_EXIT(
      {
        constexpr auto kShutdownWatchdog = std::chrono::seconds(1);
        constexpr unsigned int kChildAlarmSeconds = 5;
        ::alarm(kChildAlarmSeconds);

        FiberRuntime runtime;
        std::promise<void> shutdown_returned;
        auto shutdown_future = shutdown_returned.get_future();
        if (!runtime.Submit([&] {
              runtime.Shutdown();
              shutdown_returned.set_value();
            })) {
          std::_Exit(2);
        }

        if (shutdown_future.wait_for(kShutdownWatchdog) != std::future_status::ready) {
          std::_Exit(3);
        }
        runtime.Shutdown();
        if (runtime.IsAccepting()) {
          std::_Exit(4);
        }
        std::_Exit(0);
      },
      ::testing::ExitedWithCode(0), "");
}
#endif

TEST(FiberRuntimeTest, RunInFiberRejectsWhileGlobalRuntimeIsStopping) {
  ASSERT_TRUE(swordfs::utils::InitFiberRuntime());
  auto *runtime = swordfs::utils::ThisFiberRuntime();
  ASSERT_NE(runtime, nullptr);

  folly::fibers::Baton task_started;
  folly::fibers::Baton release_task;
  folly::fibers::Baton shutdown_done;
  ASSERT_TRUE(runtime->Submit([&] {
    task_started.post();
    release_task.wait();
  }));
  const bool task_started_in_time = swordfs::test::WaitForBaton(task_started);
  EXPECT_TRUE(task_started_in_time) << "global runtime task must start within the test watchdog";
  if (!task_started_in_time) {
    release_task.post();
  }

  std::thread shutdown([&] {
    swordfs::utils::ShutdownFiberRuntime();
    shutdown_done.post();
  });
  const bool stopped_accepting = WaitUntil([&] { return !runtime->IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "global shutdown must stop admission within the test watchdog";

  std::atomic<int> rejected{0};
  EXPECT_FALSE(swordfs::utils::RunInFiber([] {}, [&] { rejected.fetch_add(1, std::memory_order_relaxed); }));
  EXPECT_EQ(rejected.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(swordfs::utils::ThisFiberRuntime(), nullptr);

  release_task.post();
  swordfs::test::WaitForBatonOrAbort(shutdown_done, "global FiberRuntime shutdown completion after task release");
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
    folly::fibers::Baton finished;
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
    state->finished.post();
  };

  std::thread first_worker(worker, &first);
  std::thread second_worker(worker, &second);

  const bool first_initialized = swordfs::test::WaitForBaton(first.initialized);
  const bool second_initialized = swordfs::test::WaitForBaton(second.initialized);
  const bool first_task_started = swordfs::test::WaitForBaton(first.task_started);
  const bool second_task_started = swordfs::test::WaitForBaton(second.task_started);
  EXPECT_TRUE(first_initialized) << "first driver must initialize within the test watchdog";
  EXPECT_TRUE(second_initialized) << "second driver must initialize within the test watchdog";
  EXPECT_TRUE(first_task_started) << "first driver task must start within the test watchdog";
  EXPECT_TRUE(second_task_started) << "second driver task must start within the test watchdog";

  const bool setup_ok =
      first_initialized && second_initialized && first_task_started && second_task_started &&
      first.init_ok.load(std::memory_order_acquire) && second.init_ok.load(std::memory_order_acquire) &&
      first.submit_ok.load(std::memory_order_acquire) && second.submit_ok.load(std::memory_order_acquire);
  EXPECT_TRUE(first.init_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(second.init_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(first.submit_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(second.submit_ok.load(std::memory_order_acquire));
  if (!setup_ok) {
    // Every worker-side control wait is deliberately latched. Posting all of
    // them first makes timeout cleanup independent of which stage a slow
    // worker eventually reaches.
    first.release_task.post();
    second.release_task.post();
    first.check_rejection.post();
    second.check_rejection.post();
    first.restart.post();
    second.restart.post();
    first.finish.post();
    second.finish.post();
    swordfs::utils::ShutdownFiberRuntime();
    swordfs::test::WaitForBatonOrAbort(first.finished, "first FiberRuntime worker cleanup completion");
    swordfs::test::WaitForBatonOrAbort(second.finished, "second FiberRuntime worker cleanup completion");
    first_worker.join();
    second_worker.join();
    return;
  }

  auto *first_runtime = first.runtime.load(std::memory_order_acquire);
  auto *second_runtime = second.runtime.load(std::memory_order_acquire);
  EXPECT_NE(first_runtime, nullptr);
  EXPECT_NE(second_runtime, nullptr);
  EXPECT_NE(first_runtime, second_runtime);
  if (first_runtime == nullptr || second_runtime == nullptr || first_runtime == second_runtime) {
    first.release_task.post();
    second.release_task.post();
    first.check_rejection.post();
    second.check_rejection.post();
    first.restart.post();
    second.restart.post();
    first.finish.post();
    second.finish.post();
    swordfs::utils::ShutdownFiberRuntime();
    swordfs::test::WaitForBatonOrAbort(first.finished, "first FiberRuntime worker invalid-runtime cleanup");
    swordfs::test::WaitForBatonOrAbort(second.finished, "second FiberRuntime worker invalid-runtime cleanup");
    first_worker.join();
    second_worker.join();
    return;
  }

  folly::fibers::Baton shutdown_done;
  std::thread shutdown([&] {
    swordfs::utils::ShutdownFiberRuntime();
    shutdown_done.post();
  });
  const bool stopped_accepting =
      WaitUntil([&] { return !first_runtime->IsAccepting() && !second_runtime->IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "global shutdown must stop admission on every runtime within the test watchdog";

  first.check_rejection.post();
  second.check_rejection.post();
  EXPECT_TRUE(swordfs::test::WaitForBaton(first.checked_rejection))
      << "first rejection check must complete within the test watchdog";
  EXPECT_TRUE(swordfs::test::WaitForBaton(second.checked_rejection))
      << "second rejection check must complete within the test watchdog";
  EXPECT_TRUE(first.rejected_during_shutdown.load(std::memory_order_acquire));
  EXPECT_TRUE(second.rejected_during_shutdown.load(std::memory_order_acquire));

  first.release_task.post();
  second.release_task.post();
  swordfs::test::WaitForBatonOrAbort(shutdown_done, "multi-driver global shutdown completion");
  shutdown.join();

  first.restart.post();
  second.restart.post();
  EXPECT_TRUE(swordfs::test::WaitForBaton(first.restarted)) << "first driver must restart within the test watchdog";
  EXPECT_TRUE(swordfs::test::WaitForBaton(second.restarted)) << "second driver must restart within the test watchdog";
  EXPECT_TRUE(first.restart_ok.load(std::memory_order_acquire));
  EXPECT_TRUE(second.restart_ok.load(std::memory_order_acquire));
  EXPECT_NE(first.restarted_runtime.load(std::memory_order_acquire), nullptr);
  EXPECT_NE(second.restarted_runtime.load(std::memory_order_acquire), nullptr);

  swordfs::utils::ShutdownFiberRuntime();
  first.finish.post();
  second.finish.post();
  swordfs::test::WaitForBatonOrAbort(first.finished, "first FiberRuntime worker completion");
  swordfs::test::WaitForBatonOrAbort(second.finished, "second FiberRuntime worker completion");
  first_worker.join();
  second_worker.join();
}

TEST(FiberRuntimeTest, ConcurrentShutdownCallsDrainOnce) {
  FiberRuntime runtime;
  std::atomic<bool> ran{false};
  folly::fibers::Baton task_started;
  folly::fibers::Baton release_task;
  folly::fibers::Baton first_done;
  folly::fibers::Baton second_done;

  ASSERT_TRUE(runtime.Submit([&] {
    task_started.post();
    release_task.wait();
    ran.store(true, std::memory_order_release);
  }));
  const bool task_started_in_time = swordfs::test::WaitForBaton(task_started);
  EXPECT_TRUE(task_started_in_time) << "admitted task must start within the test watchdog";
  if (!task_started_in_time) {
    release_task.post();
  }

  std::thread first([&] {
    runtime.Shutdown();
    first_done.post();
  });
  std::thread second([&] {
    runtime.Shutdown();
    second_done.post();
  });
  const bool stopped_accepting = WaitUntil([&] { return !runtime.IsAccepting(); });
  EXPECT_TRUE(stopped_accepting) << "concurrent shutdown must stop admission within the test watchdog";
  release_task.post();
  swordfs::test::WaitForBatonOrAbort(first_done, "first concurrent FiberRuntime shutdown completion");
  swordfs::test::WaitForBatonOrAbort(second_done, "second concurrent FiberRuntime shutdown completion");
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
