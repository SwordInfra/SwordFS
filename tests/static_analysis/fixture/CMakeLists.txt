cmake_minimum_required(VERSION 3.16)
project(SwordFSStaticAuditFixture LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(static_audit_fixture src/Main.cpp src/Production.cpp)
target_include_directories(static_audit_fixture
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(static_audit_fixture_tests OBJECT tests/TestConsumer.cpp)
target_include_directories(static_audit_fixture_tests
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
