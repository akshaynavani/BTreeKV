cmake_minimum_required(VERSION 3.16)

project(BTreeKV VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(btreekv STATIC
  src/buffer_pool.cpp
  src/disk_manager.cpp
  src/leaf_node.cpp
  src/meta_page.cpp
  src/page_guard.cpp
  src/slotted_page.cpp
)
target_include_directories(btreekv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(btreekv PUBLIC Threads::Threads)
target_compile_options(btreekv PRIVATE -Wall -Wextra -Wpedantic)

option(BTREEKV_BUILD_TESTS "Build the GoogleTest suite" ON)
option(BTREEKV_USE_SYSTEM_GTEST "Use an installed GoogleTest instead of fetching it" OFF)

if(BTREEKV_BUILD_TESTS)
  enable_testing()
  if(BTREEKV_USE_SYSTEM_GTEST)
    find_package(GTest REQUIRED)
  else()
    include(FetchContent)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(btreekv_tests
    tests/buffer_pool_test.cpp
    tests/disk_manager_test.cpp
  )
  target_link_libraries(btreekv_tests PRIVATE btreekv GTest::gtest_main)
  target_include_directories(btreekv_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)

  include(GoogleTest)
  gtest_discover_tests(btreekv_tests DISCOVERY_TIMEOUT 30)
endif()
