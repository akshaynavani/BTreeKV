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
  src/disk_manager.cpp
)
target_include_directories(btreekv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(btreekv PUBLIC Threads::Threads)
target_compile_options(btreekv PRIVATE -Wall -Wextra -Wpedantic)
