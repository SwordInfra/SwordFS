cmake_minimum_required(VERSION 3.16)
project(SwordFS VERSION 0.1.0 LANGUAGES CXX)

# SwordFS — A modern high-performance distributed file system for AI/ML workloads

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Third-party dependencies (git submodules) ──────────────────────────

add_subdirectory(third_party/folly EXCLUDE_FROM_ALL)

# ── SwordFS target ─────────────────────────────────────────────────────

file(GLOB_RECURSE SWORDFS_SOURCES
  src/*.cpp
  src/*.h
)

add_library(swordfs ${SWORDFS_SOURCES})
target_link_libraries(swordfs PUBLIC folly)
target_include_directories(swordfs PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
