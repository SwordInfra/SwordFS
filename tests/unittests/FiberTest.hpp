// Copyright 2026 SwordFS Contributors.
// Licensed under the Apache License, Version 2.0.

#pragma once

#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManagerInternal.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>
#include <gtest/gtest.h>

#include <exception>
#include <functional>
#include <thread>
#include <utility>

#include "utils/BlockingExecutor.hpp"

namespace swordfs::test {

// Execute a unit-test body in a real Folly fiber. If a helper is invoked from
// an already-running fiber, preserve that execution context instead of nesting
// another EventBase/FiberManager.
template <typename Fn>
void RunInTestFiber(Fn &&fn) {
  if (folly::fibers::onFiber()) {
    std::forward<Fn>(fn)();
    return;
  }

  folly::EventBase evb;
  auto &manager = folly::fibers::getFiberManager(evb);
  folly::fibers::Baton done;
  std::exception_ptr exception;

  manager.addTask([&] {
    try {
      std::forward<Fn>(fn)();
    } catch (...) {
      exception = std::current_exception();
    }
    done.post();
  });

  while (!done.try_wait()) {
    evb.loopOnce();
  }
  if (exception != nullptr) {
    std::rethrow_exception(exception);
  }
}

inline utils::BlockingExecutor &BlockingTestExecutor() {
  static utils::BlockingExecutor executor(2);
  return executor;
}

// Explicit fiber -> POSIX-thread transition for test-only blocking operations.
// The helper deliberately has no auto-detection fallback: callers must already
// be in the fiber domain or the BlockingExecutor contract fails in Debug.
template <typename Fn>
decltype(auto) RunInTestThreadFromFiber(Fn &&fn) {
  return BlockingTestExecutor().RunFromFiber(std::forward<Fn>(fn));
}

template <typename Fn, typename... Args>
std::thread StartFiberTestThread(Fn &&fn, Args &&...args) {
  return std::thread([fn = std::forward<Fn>(fn), ... args = std::forward<Args>(args)]() mutable {
    RunInTestFiber([&] { std::invoke(fn, args...); });
  });
}

}  // namespace swordfs::test

// GoogleTest owns fixture construction/SetUp/TearDown on the normal test
// thread. FIBER_TEST[_F] changes only TestBody execution, which is where
// fiber-owned production APIs are exercised. Fixtures whose SetUp/TearDown
// themselves call fiber-only APIs should use RunInTestFiber explicitly there.
#define SWORDFS_FIBER_TEST_(test_suite_name, test_name, parent_class, parent_id)                                       \
  static_assert(sizeof(GTEST_STRINGIFY_(test_suite_name)) > 1, "test_suite_name must not be empty");                   \
  static_assert(sizeof(GTEST_STRINGIFY_(test_name)) > 1, "test_name must not be empty");                               \
  class GTEST_TEST_CLASS_NAME_(test_suite_name, test_name) : public parent_class {                                     \
   public:                                                                                                             \
    GTEST_TEST_CLASS_NAME_(test_suite_name, test_name)() = default;                                                    \
    ~GTEST_TEST_CLASS_NAME_(test_suite_name, test_name)() override = default;                                          \
    GTEST_TEST_CLASS_NAME_(test_suite_name,                                                                            \
                           test_name)(const GTEST_TEST_CLASS_NAME_(test_suite_name, test_name) &) = delete;            \
    GTEST_TEST_CLASS_NAME_(test_suite_name, test_name) &operator=(const GTEST_TEST_CLASS_NAME_(test_suite_name,        \
                                                                                               test_name) &) = delete; \
    GTEST_TEST_CLASS_NAME_(test_suite_name,                                                                            \
                           test_name)(GTEST_TEST_CLASS_NAME_(test_suite_name, test_name) &&) noexcept = delete;        \
    GTEST_TEST_CLASS_NAME_(test_suite_name, test_name) &operator=(                                                     \
        GTEST_TEST_CLASS_NAME_(test_suite_name, test_name) &&) noexcept = delete;                                      \
                                                                                                                       \
   private:                                                                                                            \
    void TestBody() override {                                                                                         \
      (void)::swordfs::test::BlockingTestExecutor();                                                                   \
      ::swordfs::test::RunInTestFiber([this] { FiberTestBody(); });                                                    \
    }                                                                                                                  \
    void FiberTestBody();                                                                                              \
    [[maybe_unused]] static ::testing::TestInfo *const test_info_;                                                     \
  };                                                                                                                   \
  ::testing::TestInfo *const GTEST_TEST_CLASS_NAME_(test_suite_name, test_name)::test_info_ =                          \
      ::testing::internal::MakeAndRegisterTestInfo(                                                                    \
          #test_suite_name, #test_name, nullptr, nullptr, ::testing::internal::CodeLocation(__FILE__, __LINE__),       \
          (parent_id), ::testing::internal::SuiteApiResolver<parent_class>::GetSetUpCaseOrSuite(__FILE__, __LINE__),   \
          ::testing::internal::SuiteApiResolver<parent_class>::GetTearDownCaseOrSuite(__FILE__, __LINE__),             \
          new ::testing::internal::TestFactoryImpl<GTEST_TEST_CLASS_NAME_(test_suite_name, test_name)>);               \
  void GTEST_TEST_CLASS_NAME_(test_suite_name, test_name)::FiberTestBody()

#define FIBER_TEST(test_suite_name, test_name) \
  SWORDFS_FIBER_TEST_(test_suite_name, test_name, ::testing::Test, ::testing::internal::GetTestTypeId())

#define FIBER_TEST_F(test_fixture, test_name) \
  SWORDFS_FIBER_TEST_(test_fixture, test_name, test_fixture, ::testing::internal::GetTypeId<test_fixture>())
