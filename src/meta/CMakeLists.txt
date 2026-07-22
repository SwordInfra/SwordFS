# SwordFS metadata engine library — static library with unit tests.
#
# swordfs_meta: the core metadata engine (IMetadataEngine + MemEngine + KeyBuilder).
# swordfs_meta_test: unit tests for the metadata engine.

add_library(swordfs_meta STATIC
  KeyBuilder.cpp
  MemEngine.cpp
)

target_include_directories(swordfs_meta PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_BINARY_DIR}/../..
)

target_link_libraries(swordfs_meta PUBLIC
  ${CMAKE_DL_LIBS}
)

target_compile_definitions(swordfs_meta PRIVATE _FILE_OFFSET_BITS=64)

# ── Unit tests ──────────────────────────────────────────────────────────

enable_testing()

add_executable(swordfs_meta_test
  tests/MemEngine_test.cpp
)

target_link_libraries(swordfs_meta_test PRIVATE
  swordfs_meta
)

target_include_directories(swordfs_meta_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_test(NAME MemEngineTest COMMAND swordfs_meta_test)
