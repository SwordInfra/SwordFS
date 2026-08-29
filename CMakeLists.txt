cmake_minimum_required(VERSION 3.16)
project(
  SwordFS
  VERSION 0.1.0
  LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# folly's config uses find_package(Boost) which triggers CMP0167 (FindBoost
# removed in CMake >= 3.30).  Use the NEW policy to suppress the developer
# warning.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# ============================================================
# Third-party dependencies
# ============================================================

# Exclude conda-provided libraries from CMake search paths to prevent RUNPATH
# pollution (e.g. /opt/conda/lib/libcurl.so.4 lacks version info).
list(FILTER CMAKE_PREFIX_PATH EXCLUDE REGEX "/opt/conda")
list(FILTER CMAKE_SYSTEM_PREFIX_PATH EXCLUDE REGEX "/opt/conda")

# folly (install via: ./scripts/install-deps.sh)
find_package(folly REQUIRED)

# fuse3 (system package, install via: apt install libfuse3-dev)
find_library(FUSE3_LIBRARY fuse3 REQUIRED)
find_path(FUSE3_INCLUDE_DIR fuse.h PATH_SUFFIXES fuse3)

# Redis metadata backend. Prefer an installed redis-plus-plus package, but fetch
# a pinned release for developer environments without it.
find_package(redis++ CONFIG QUIET)
if(TARGET redis++::redis++)
  set(REDIS_PLUS_PLUS_TARGET redis++::redis++)
elseif(TARGET redis++::redis++_static)
  set(REDIS_PLUS_PLUS_TARGET redis++::redis++_static)
else()
  # hiredis and redis-plus-plus still declare compatibility with very old CMake
  # releases. CMake 4 requires an explicit policy compatibility floor.
  set(_swordfs_saved_policy_version_minimum "${CMAKE_POLICY_VERSION_MINIMUM}")
  set(_swordfs_had_policy_version_minimum FALSE)
  if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
    set(_swordfs_had_policy_version_minimum TRUE)
  endif()
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  FetchContent_Declare(
    hiredis
    GIT_REPOSITORY https://github.com/redis/hiredis.git
    GIT_TAG 60e5075d4ac77424809f855ba3e398df7aacefe8)
  FetchContent_MakeAvailable(hiredis)
  # Feed the FetchContent-provided hiredis target to redis-plus-plus's
  # FindHiredis.cmake fallback path. Hiredis 1.2 keeps headers at its source
  # root while redis-plus-plus expects an include root containing hiredis/.
  file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/redis-hiredis-include)
  file(CREATE_LINK ${hiredis_SOURCE_DIR}
       ${CMAKE_BINARY_DIR}/redis-hiredis-include/hiredis SYMBOLIC)
  set(HIREDIS_HEADER
      ${CMAKE_BINARY_DIR}/redis-hiredis-include
      CACHE PATH "" FORCE)
  set(HIREDIS_LIB
      hiredis::hiredis
      CACHE STRING "" FORCE)
  FetchContent_Declare(
    redis_plus_plus
    GIT_REPOSITORY https://github.com/sewenew/redis-plus-plus.git
    GIT_TAG a63ac43bf192772910b52e27cd2b42a6098a0071)
  set(REDIS_PLUS_PLUS_CXX_STANDARD
      20
      CACHE STRING "" FORCE)
  set(REDIS_PLUS_PLUS_BUILD_TEST
      OFF
      CACHE BOOL "" FORCE)
  set(REDIS_PLUS_PLUS_BUILD_EXAMPLES
      OFF
      CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(redis_plus_plus)
  set(REDIS_PLUS_PLUS_TARGET redis++_static)
  if(_swordfs_had_policy_version_minimum)
    set(CMAKE_POLICY_VERSION_MINIMUM "${_swordfs_saved_policy_version_minimum}")
  else()
    unset(CMAKE_POLICY_VERSION_MINIMUM)
  endif()
  unset(_swordfs_saved_policy_version_minimum)
  unset(_swordfs_had_policy_version_minimum)
endif()

if(TARGET hiredis::hiredis)
  set(REDIS_PLUS_PLUS_HIREDIS_TARGET hiredis::hiredis)
endif()

# ============================================================
# Version header
# ============================================================

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/include/SwordfsVersion.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/SwordfsVersion.h)

# ============================================================
# S3 data engine
# ============================================================

set(OPENSSL_USE_STATIC_LIBS OFF)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS s3)

# jitterentropy is a transitive dependency of the AWS SDK only when it is built
# against static OpenSSL (libcrypto.a).  When the SDK links shared OpenSSL the
# symbol is resolved internally and this library is not needed.  Make it
# optional so that both local dev environments (where the pre-built SDK may use
# static OpenSSL) and CI (where it uses shared) work without patching.
find_library(JITTERENTROPY_LIB jitterentropy)

# libzstd is a transitive dependency of static libcrypto.a (OpenSSL) when it was
# built with zstd compression support.
find_library(ZSTD_LIB zstd)

# ============================================================
# SwordFS library (all sources except main)
# ============================================================

file(GLOB_RECURSE SWORDFS_SOURCES src/*.cpp src/*.h)
list(FILTER SWORDFS_SOURCES EXCLUDE REGEX "src/cmd/Main\.cpp$")

add_library(swordfs_lib OBJECT ${SWORDFS_SOURCES})
target_link_libraries(
  swordfs_lib PUBLIC ${CMAKE_DL_LIBS} ${AWSSDK_LINK_LIBRARIES} Folly::folly
                     ${FUSE3_LIBRARY} ${REDIS_PLUS_PLUS_TARGET})
if(REDIS_PLUS_PLUS_HIREDIS_TARGET)
  target_link_libraries(swordfs_lib PUBLIC ${REDIS_PLUS_PLUS_HIREDIS_TARGET})
endif()
if(JITTERENTROPY_LIB)
  target_link_libraries(swordfs_lib PUBLIC ${JITTERENTROPY_LIB})
endif()
if(ZSTD_LIB)
  target_link_libraries(swordfs_lib PUBLIC ${ZSTD_LIB})
endif()
target_include_directories(
  swordfs_lib
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}
         ${FUSE3_INCLUDE_DIR}
         ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/include)
target_compile_definitions(swordfs_lib PRIVATE SWORDFS_ENABLE_S3
                                               _FILE_OFFSET_BITS=64)

# ============================================================
# SwordFS binary
# ============================================================

add_executable(swordfs src/cmd/Main.cpp)
target_link_libraries(swordfs PRIVATE swordfs_lib)

# ============================================================
# Tests
# ============================================================

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0)
set(gtest_force_shared_crt
    ON
    CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Unit tests (tests/unittests/)
file(GLOB_RECURSE TEST_SOURCES tests/unittests/*.cpp)
add_executable(swordfs_test ${TEST_SOURCES})
target_link_libraries(swordfs_test PRIVATE swordfs_lib gtest gtest_main)
# Ensure FetchContent gtest headers take priority over any system/conda gtest
target_include_directories(
  swordfs_test BEFORE PRIVATE ${googletest_SOURCE_DIR}/googletest/include
                              ${googletest_SOURCE_DIR}/googletest)
target_compile_definitions(swordfs_test PRIVATE _FILE_OFFSET_BITS=64)

include(GoogleTest)
gtest_discover_tests(swordfs_test)

# End-to-end tests (tests/e2e/)
file(GLOB_RECURSE E2E_TEST_SOURCES tests/e2e/*.cpp)
add_executable(swordfs_e2e_test ${E2E_TEST_SOURCES})
target_link_libraries(swordfs_e2e_test PRIVATE swordfs_lib gtest gtest_main)
target_include_directories(
  swordfs_e2e_test BEFORE
  PRIVATE ${googletest_SOURCE_DIR}/googletest/include
          ${googletest_SOURCE_DIR}/googletest
          ${CMAKE_CURRENT_SOURCE_DIR} # for "tests/e2e/Fixture.hpp" includes
)
target_compile_definitions(swordfs_e2e_test PRIVATE _FILE_OFFSET_BITS=64)

gtest_discover_tests(swordfs_e2e_test)
