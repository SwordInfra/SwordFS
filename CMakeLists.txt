cmake_minimum_required(VERSION 3.16)
project(SwordFS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# folly's config uses find_package(Boost) which triggers CMP0167
# (FindBoost removed in CMake >= 3.30).  Use the NEW policy to
# suppress the developer warning.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# ============================================================
# Third-party dependencies
# ============================================================

# Exclude conda-provided libraries from CMake search paths to prevent
# RUNPATH pollution (e.g. /opt/conda/lib/libcurl.so.4 lacks version info).
list(FILTER CMAKE_PREFIX_PATH EXCLUDE REGEX "/opt/conda")
list(FILTER CMAKE_SYSTEM_PREFIX_PATH EXCLUDE REGEX "/opt/conda")

# folly (install via: ./scripts/install-deps.sh)
find_package(folly REQUIRED)

# fuse3 (system package, install via: apt install libfuse3-dev)
find_library(FUSE3_LIBRARY fuse3 REQUIRED)
find_path(FUSE3_INCLUDE_DIR fuse.h PATH_SUFFIXES fuse3)

# ============================================================
# Version header
# ============================================================

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/SwordfsVersion.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/SwordfsVersion.h
)

# ============================================================
# S3 data engine
# ============================================================

set(OPENSSL_USE_STATIC_LIBS OFF)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(AWSSDK REQUIRED COMPONENTS s3)

# jitterentropy is a transitive dependency of the AWS SDK only when
# it is built against static OpenSSL (libcrypto.a).  When the SDK
# links shared OpenSSL the symbol is resolved internally and this
# library is not needed.  Make it optional so that both local dev
# environments (where the pre-built SDK may use static OpenSSL)
# and CI (where it uses shared) work without patching.
find_library(JITTERENTROPY_LIB jitterentropy)

# ============================================================
# SwordFS library (all sources except main)
# ============================================================

file(GLOB_RECURSE SWORDFS_SOURCES
  src/*.cpp
  src/*.h
)
list(FILTER SWORDFS_SOURCES EXCLUDE REGEX "src/cmd/Main\.cpp$")

add_library(swordfs_lib OBJECT ${SWORDFS_SOURCES})
target_link_libraries(swordfs_lib PUBLIC
  ${CMAKE_DL_LIBS}
  ${AWSSDK_LINK_LIBRARIES}
  Folly::folly
  ${FUSE3_LIBRARY}
)
if(JITTERENTROPY_LIB)
  target_link_libraries(swordfs_lib PUBLIC ${JITTERENTROPY_LIB})
endif()
target_include_directories(swordfs_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${FUSE3_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/include
)
target_compile_definitions(swordfs_lib PRIVATE SWORDFS_ENABLE_S3 _FILE_OFFSET_BITS=64)

# ============================================================
# SwordFS binary
# ============================================================

add_executable(swordfs src/cmd/Main.cpp)
target_link_libraries(swordfs PRIVATE swordfs_lib)

# ============================================================
# Tests
# ============================================================

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

file(GLOB_RECURSE TEST_SOURCES tests/*.cpp)
add_executable(swordfs_test ${TEST_SOURCES})
target_link_libraries(swordfs_test PRIVATE swordfs_lib gtest gtest_main)
# Ensure FetchContent gtest headers take priority over any system/conda gtest
target_include_directories(swordfs_test BEFORE PRIVATE
  ${googletest_SOURCE_DIR}/googletest/include
  ${googletest_SOURCE_DIR}/googletest
)
target_compile_definitions(swordfs_test PRIVATE _FILE_OFFSET_BITS=64)

include(GoogleTest)
gtest_discover_tests(swordfs_test)
