cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(ujrpc LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

# Make Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CACHEFILE_DIR "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to static libs")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to shared libs")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/bin")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")

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

include_directories(include)
add_library(ujrpc_server src/engine_libc.cpp)
target_link_libraries(ujrpc_server simdjson::simdjson Threads::Threads)

add_executable(ujrpc_server_bench benchmark/ujrpc_server.cpp)
target_link_libraries(ujrpc_server_bench ujrpc_server)

add_executable(ujrpc_client_bench benchmark/ujrpc_client.cpp)
target_link_libraries(ujrpc_client_bench benchmark::benchmark)

find_package(Python REQUIRED COMPONENTS Interpreter Development)
include_directories(${Python_INCLUDE_DIRS})
Python_add_library(py_ujrpc src/python.c)
target_include_directories(py_ujrpc PUBLIC src/ include/)
target_link_libraries(py_ujrpc PRIVATE ujrpc_server)
set_target_properties(py_ujrpc PROPERTIES OUTPUT_NAME ujrpc)

