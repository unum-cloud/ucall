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
include(ExternalProject)

FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/ashvardanian/simdjson
    GIT_SHALLOW 1
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

set(PICO_DIR ${CMAKE_BINARY_DIR}/_deps/picohttpparser)
ExternalProject_Add(
    picohttpparser
    GIT_REPOSITORY https://github.com/h2o/picohttpparser
    PREFIX ${CMAKE_BINARY_DIR}/_deps/
    SOURCE_DIR ${PICO_DIR}
    CONFIGURE_COMMAND echo Configuring PicoHTTPParse
    BUILD_COMMAND cd ${PICO_DIR} && gcc -c picohttpparser.c && ar rcs libpicohttpparser.a picohttpparser.o
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
)
add_library(libpicohttpparser STATIC IMPORTED GLOBAL)
set_property(TARGET libpicohttpparser
    PROPERTY IMPORTED_LOCATION
    ${PICO_DIR}/libpicohttpparser.a
)

include_directories(${PICO_DIR})
set(picohttpparser_LIBS ${PICO_DIR}/libpicohttpparser.a)

find_package(Threads REQUIRED)

include_directories(include/ src/)

add_library(ujrpc_server_uring src/engine_uring.cpp)
target_link_libraries(ujrpc_server_uring simdjson::simdjson Threads::Threads uring)

add_library(ujrpc_server_posix src/engine_posix.cpp)
target_link_libraries(ujrpc_server_posix simdjson::simdjson Threads::Threads)

add_executable(ujrpc_server_bench benchmark/ujrpc_server.cpp)
target_link_libraries(ujrpc_server_bench ujrpc_server_posix ${picohttpparser_LIBS})

add_executable(ujrpc_client_bench benchmark/ujrpc_client.cpp)
target_link_libraries(ujrpc_client_bench benchmark::benchmark)
