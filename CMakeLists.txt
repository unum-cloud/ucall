cmake_minimum_required(VERSION 3.14 FATAL_ERROR)
project(ujrpc LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(UJRPC_BUILD_BENCHMARKS "Builds all available backend for the summation server to run benchmarks" ON)
option(UJRPC_BUILD_EXAMPLES "Builds examples for Redis-like server and PyTorch deployment")


# Make Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CACHEFILE_DIR "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to static libs")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to shared libs")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/bin")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")

# Check if we are running on Linux
if(UNIX AND NOT APPLE)
    set(LINUX TRUE)
endif()

# Pull the version of Linux kernel, to check if io_uring is available
if(LINUX)
    execute_process(COMMAND uname -r OUTPUT_VARIABLE UNAME_RESULT OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(-- " Kernel version: " ${UNAME_RESULT})
    string(REGEX MATCH "[0-9]+.[0-9]+" LINUX_KERNEL_VERSION ${UNAME_RESULT})
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

# On MacOS you may need to locate headers here:
# export CPATH=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/
set(PICOHTTP_DIR ${CMAKE_BINARY_DIR}/_deps/picohttpparser-ep)
ExternalProject_Add(
    picohttpparser-ep
    GIT_REPOSITORY https://github.com/h2o/picohttpparser
    PREFIX ${CMAKE_BINARY_DIR}/_deps/
    SOURCE_DIR ${PICOHTTP_DIR}
    CONFIGURE_COMMAND echo Configuring PicoHTTPParse
    BUILD_COMMAND cd ${PICOHTTP_DIR} && ${CMAKE_C_COMPILER} -c picohttpparser.c && ar rcs libpicohttpparser.a picohttpparser.o
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
)
add_library(picohttp_internal STATIC IMPORTED GLOBAL)
add_dependencies(picohttp_internal picohttpparser-ep)
set_property(TARGET picohttp_internal
    PROPERTY IMPORTED_LOCATION
    ${PICOHTTP_DIR}/libpicohttpparser.a
)
include_directories(${PICOHTTP_DIR})
set(PICOHTTP_LIBS picohttp_internal)

if(LINUX)
    set(URING_DIR ${CMAKE_BINARY_DIR}/_deps/liburing-ep)
    ExternalProject_Add(
        liburing-ep
        GIT_REPOSITORY https://github.com/axboe/liburing
        PREFIX ${CMAKE_BINARY_DIR}/_deps/
        SOURCE_DIR ${URING_DIR}
        CONFIGURE_COMMAND echo Configuring LibUring && cd ${URING_DIR} && ./configure --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER};
        BUILD_COMMAND cd ${URING_DIR} && make;
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
    )
    add_library(uring_internal STATIC IMPORTED GLOBAL)
    add_dependencies(uring_internal liburing-ep)
    set_property(TARGET uring_internal
        PROPERTY IMPORTED_LOCATION
        ${URING_DIR}/src/liburing.a
    )

    include_directories(${URING_DIR}/src/include/)
    set(URING_LIBS uring_internal)
endif()

find_package(Threads REQUIRED)
include_directories(include/ src/)

add_library(ujrpc_server_posix src/engine_posix.cpp)
target_link_libraries(ujrpc_server_posix simdjson::simdjson Threads::Threads ${PICOHTTP_LIBS})

add_executable(ujrpc_example_sum_posix examples/sum/ujrpc_server.cpp)
target_link_libraries(ujrpc_example_sum_posix ujrpc_server_posix)

if(LINUX AND LINUX_KERNEL_VERSION VERSION_GREATER_EQUAL 5.19)
    add_library(ujrpc_server_uring src/engine_uring.cpp)
    target_link_libraries(ujrpc_server_uring simdjson::simdjson Threads::Threads ${PICOHTTP_LIBS} ${URING_LIBS})
    add_executable(ujrpc_example_sum_uring examples/sum/ujrpc_server.cpp)
    target_link_libraries(ujrpc_example_sum_uring ujrpc_server_uring)
else()
    message("Linux kernel version should be greater than 5.19 to use io_uring features")
endif()

if(UJRPC_BUILD_EXAMPLES)
    add_executable(ujrpc_example_redis examples/redis/ujrpc_server.cpp)
    target_link_libraries(ujrpc_example_redis ujrpc_server_posix)

    find_package(Torch)
    add_executable(ujrpc_example_pytorcs examples/pytorch/ujrpc_server.cpp)
    target_link_libraries(ujrpc_example_pytorcs ujrpc_server_posix "${TORCH_LIBRARIES}")
endif()

find_package(Python REQUIRED COMPONENTS Interpreter Development)
include_directories(${Python_INCLUDE_DIRS})
Python_add_library(py_ujrpc src/python.c)
target_include_directories(py_ujrpc PUBLIC src/ include/)
target_link_libraries(py_ujrpc PRIVATE ujrpc_server_posix)
set_target_properties(py_ujrpc PROPERTIES OUTPUT_NAME ujrpc)