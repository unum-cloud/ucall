cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(ujrpc LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Make Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson
    GIT_TAG v3.0.0
)
FetchContent_MakeAvailable(simdjson)
include_directories(${simdjson_SOURCE_DIR}/include)

set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_DOXYGEN OFF)
set(BENCHMARK_INSTALL_DOCS OFF)
set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
set(BENCHMARK_USE_BUNDLED_GTEST ON)
FetchContent_Declare(
    benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.7.0
)
FetchContent_MakeAvailable(benchmark)
include_directories(${benchmark_SOURCE_DIR}/include)

find_package(Threads REQUIRED)

include_directories(include/ src/)

add_library(ujrpc_server src/engine_uring.cpp src/helpers/receive.cpp)
target_link_libraries(ujrpc_server simdjson::simdjson Threads::Threads)

add_library(ujrpc_server_sync src/engine_posix.cpp src/helpers/receive.cpp)
target_link_libraries(ujrpc_server_sync simdjson::simdjson Threads::Threads)

add_executable(ujrpc_server_bench benchmark/ujrpc_server.cpp)
target_link_libraries(ujrpc_server_bench ujrpc_server)

add_executable(ujrpc_client_bench benchmark/ujrpc_client.cpp)
target_link_libraries(ujrpc_client_bench benchmark::benchmark)
