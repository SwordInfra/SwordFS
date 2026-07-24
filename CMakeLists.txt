cmake_minimum_required(VERSION 3.16)
project(SwordFS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ============================================================
# Third-party dependencies
# ============================================================

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
# S3 data engine (optional)
# ============================================================

option(ENABLE_S3 "Enable S3 object storage backend" OFF)

# Framework code — always compiled
add_library(swordfs_storage OBJECT
  src/storage/StorageRegistry.cpp
)
target_link_libraries(swordfs_storage PUBLIC ${CMAKE_DL_LIBS})
target_include_directories(swordfs_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(ENABLE_S3)
  find_package(AWSSDK REQUIRED COMPONENTS s3)
  target_sources(swordfs_storage PRIVATE
    src/storage/s3/S3DataEngine.cpp
  )
  target_link_libraries(swordfs_storage PUBLIC
    ${AWSSDK_LINK_LIBRARIES}
    Aws::aws-cpp-sdk-s3
  )
  target_include_directories(swordfs_storage PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_definitions(swordfs_storage PRIVATE
    SWORDFS_ENABLE_S3
    _FILE_OFFSET_BITS=64
  )
endif()

# ============================================================
# SwordFS library (all sources except main)
# ============================================================

file(GLOB_RECURSE SWORDFS_SOURCES
  src/*.cpp
  src/*.h
)
list(FILTER SWORDFS_SOURCES EXCLUDE REGEX "src/cmd/Main\.cpp$")
list(FILTER SWORDFS_SOURCES EXCLUDE REGEX "src/storage/s3/")

add_library(swordfs_lib OBJECT ${SWORDFS_SOURCES})
if(ENABLE_S3)
  target_link_libraries(swordfs_lib PUBLIC swordfs_storage)
endif()
target_link_libraries(swordfs_lib PUBLIC ${CMAKE_DL_LIBS} Folly::folly ${FUSE3_LIBRARY})
target_include_directories(swordfs_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${FUSE3_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/include
)
target_compile_definitions(swordfs_lib PRIVATE _FILE_OFFSET_BITS=64)

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
