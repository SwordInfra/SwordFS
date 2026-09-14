# Copyright 2026 SwordFS Contributors. Licensed under the Apache License,
# Version 2.0.

if(NOT DEFINED SOURCE_ROOT)
  message(FATAL_ERROR "SOURCE_ROOT is required")
endif()

file(
  GLOB_RECURSE SWORDFS_SOURCE_FILES
  LIST_DIRECTORIES FALSE
  "${SOURCE_ROOT}/src/*.cpp" "${SOURCE_ROOT}/src/*.h"
  "${SOURCE_ROOT}/src/*.hpp")

set(RAW_BLOCKING_SYNC_TYPES
    "std::mutex"
    "std::timed_mutex"
    "std::recursive_mutex"
    "std::recursive_timed_mutex"
    "std::shared_mutex"
    "std::shared_timed_mutex"
    "std::condition_variable"
    "std::condition_variable_any"
    "pthread_mutex_t"
    "pthread_rwlock_t"
    "boost::mutex"
    "boost::shared_mutex"
    "folly::SharedMutex"
    "folly::RWSpinLock"
    "folly::MicroSpinLock"
    "folly::SpinLock"
    "folly::fibers::TimedMutex"
    "folly::fibers::TimedRWMutex")

set(violations "")

foreach(source_file IN LISTS SWORDFS_SOURCE_FILES)
  file(RELATIVE_PATH relative_path "${SOURCE_ROOT}" "${source_file}")
  file(READ "${source_file}" source_text)

  if(NOT relative_path STREQUAL "src/utils/Synchronization.hpp")
    foreach(type_name IN LISTS RAW_BLOCKING_SYNC_TYPES)
      string(FIND "${source_text}" "${type_name}" match_pos)
      if(NOT match_pos EQUAL -1)
        list(
          APPEND
          violations
          "${relative_path}: direct use of ${type_name}, choose utils::FiberMutex/FiberRWMutex or utils::ThreadMutex for the correct execution domain"
        )
      endif()
    endforeach()

    string(FIND "${source_text}" "HybridMutex" hybrid_mutex_pos)
    if(NOT hybrid_mutex_pos EQUAL -1)
      list(
        APPEND
        violations
        "${relative_path}: HybridMutex is forbidden; redesign cross-domain coordination with atomics plus FiberBaton"
      )
    endif()
  endif()

  string(FIND "${source_text}" "sw::redis::" redis_api_pos)
  if(NOT redis_api_pos EQUAL -1
     AND NOT relative_path STREQUAL "src/metadata/redis/RedisMetaClient.cpp"
     AND NOT relative_path STREQUAL "src/metadata/redis/RedisMetaClient.hpp"
     AND NOT relative_path STREQUAL "src/metadata/redis/RedisKvTxn.cpp"
     AND NOT relative_path STREQUAL "src/metadata/redis/RedisKvTxn.hpp")
    list(
      APPEND
      violations
      "${relative_path}: direct redis++ usage is outside the Redis blocking-I/O boundary; route Redis work through RedisMetaClient and an explicit BlockingExecutor transition"
    )
  endif()

  string(FIND "${source_text}" "Aws::S3::" s3_api_pos)
  if(NOT s3_api_pos EQUAL -1
     AND NOT relative_path STREQUAL "src/storage/s3/S3DataEngine.cpp"
     AND NOT relative_path STREQUAL "src/storage/s3/S3DataEngine.hpp")
    list(
      APPEND
      violations
      "${relative_path}: direct AWS S3 SDK usage is outside the S3 blocking-I/O boundary; route S3 work through S3DataEngine and an explicit BlockingExecutor transition"
    )
  endif()
endforeach()

if(violations)
  list(JOIN violations "\n  - " violation_text)
  message(
    FATAL_ERROR
      "Fiber-safety execution-domain check failed:\n  - ${violation_text}\n"
      "Use semantic synchronization wrappers and keep blocking backends behind their thread-domain boundaries."
  )
endif()

message(STATUS "Fiber-safety execution-domain check passed")
