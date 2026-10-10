# SPDX-License-Identifier: Apache-2.0
#
# Google Benchmark harness for the platform-neutral parsing and serialization
# code paths. The CodSpeed fork of Google Benchmark behaves like upstream when
# CODSPEED_MODE is unset and enables CodSpeed instrumentation otherwise.

cmake_minimum_required(VERSION 3.16)
project(memftprt_benchmarks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

include(FetchContent)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON)
set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  google_benchmark
  GIT_REPOSITORY https://github.com/CodSpeedHQ/codspeed-cpp
  SOURCE_SUBDIR google_benchmark
  GIT_TAG v2.4.0
)
FetchContent_MakeAvailable(google_benchmark)

set(MEMFTPRT_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

add_executable(memftprt_bench
  memftprt_bench.cpp
  ${MEMFTPRT_SOURCE_DIR}/memory_report.cpp
  ${MEMFTPRT_SOURCE_DIR}/runtime/hotspot_nmt.cpp
  ${MEMFTPRT_SOURCE_DIR}/linux/proc_memory.cpp
  ${MEMFTPRT_SOURCE_DIR}/linux/working_set.cpp
)
target_include_directories(memftprt_bench PRIVATE ${MEMFTPRT_SOURCE_DIR})
target_link_libraries(memftprt_bench PRIVATE benchmark::benchmark)
