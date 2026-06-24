cmake_minimum_required(VERSION 3.16)
project(SwordFS VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Third-party dependencies ────────────────────────────────────────────
# folly (git submodule)
add_subdirectory(third_party/folly EXCLUDE_FROM_ALL)

# fuse3 (system package, install via: apt install libfuse3-dev)
find_library(FUSE3_LIBRARY fuse3 REQUIRED)
find_path(FUSE3_INCLUDE_DIR fuse.h PATH_SUFFIXES fuse3)

# ── Version header ─────────────────────────────────────────────────────

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/include/SwordfsVersion.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/SwordfsVersion.h
)

# ── SwordFS binary ─────────────────────────────────────────────────────

file(GLOB_RECURSE SWORDFS_SOURCES
  src/*.cpp
  src/*.h
)

add_executable(swordfs ${SWORDFS_SOURCES})
target_link_libraries(swordfs PRIVATE ${CMAKE_DL_LIBS} folly ${FUSE3_LIBRARY})
target_include_directories(swordfs PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${FUSE3_INCLUDE_DIR}
)
target_compile_definitions(swordfs PRIVATE _FILE_OFFSET_BITS=64)
