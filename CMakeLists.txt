cmake_minimum_required(VERSION 3.20 FATAL_ERROR)
project(
    ucall
    VERSION 0.5.4
    LANGUAGES C CXX
    DESCRIPTION "Up to 100x Faster FastAPI. JSON-RPC with io_uring, SIMD-acceleration, and pure CPython bindings"
    HOMEPAGE_URL "https://github.com/unum-cloud/ucall")

set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

option(UCALL_BUILD_BENCHMARKS "Builds all available backend for the summation server to run benchmarks" OFF)
option(UCALL_BUILD_EXAMPLES "Builds examples for Redis-like server and PyTorch deployment")
message("CMAKE_SYSTEM_NAME: ${CMAKE_SYSTEM_NAME}")

# Make Release by default
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CACHEFILE_DIR "${CMAKE_SOURCE_DIR}/build")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to static libs")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/lib" CACHE PATH "Path to shared libs")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/build/bin")

if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W3")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /DEBUG")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2")
    set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} /O2 /DEBUG")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pedantic")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fmax-errors=1")
endif()

# Check if we are running on Linux
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LINUX TRUE)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11")
    set(CMAKE_OSX_SYSROOT CACHE STRING "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11" CACHE STRING "Minimum OS X deployment version")
    set(CMAKE_OSX_ARCHITECTURES "x86_64" "universal2" "arm64" CACHE STRING "Minimum OS X deployment version")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -std=c++17")
endif()

# Pull the version of Linux kernel, to check if io_uring is available
if(LINUX)
    execute_process(COMMAND uname -r OUTPUT_VARIABLE UNAME_RESULT OUTPUT_STRIP_TRAILING_WHITESPACE)
    message(-- " Linux Kernel version: " ${UNAME_RESULT})
    string(REGEX MATCH "[0-9]+.[0-9]+" LINUX_KERNEL_VERSION ${UNAME_RESULT})
endif()

include(FetchContent)
include(ExternalProject)

FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG v3.1.6
    GIT_SHALLOW 1
)
FetchContent_MakeAvailable(simdjson)
include_directories(${simdjson_SOURCE_DIR}/include)

if(${UCALL_BUILD_BENCHMARKS})
    set(BENCHMARK_ENABLE_TESTING OFF)
    set(BENCHMARK_ENABLE_INSTALL OFF)
    set(BENCHMARK_ENABLE_DOXYGEN OFF)
    set(BENCHMARK_INSTALL_DOCS OFF)
    set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
    set(BENCHMARK_USE_BUNDLED_GTEST ON)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark
        GIT_TAG v1.7.0
        GIT_SHALLOW 1
    )
    FetchContent_MakeAvailable(benchmark)
    include_directories(${benchmark_SOURCE_DIR}/include)
endif()

# CLI
FetchContent_Declare(
    cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG v3.1.1
    GIT_SHALLOW 1
)
FetchContent_MakeAvailable(cxxopts)
include_directories(${cxxopts_SOURCE_DIR}/include)

# Parsing HTTP headers
# On MacOS you may need to locate headers here:
# export CPATH=/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/
FetchContent_Declare(
    picohttpparser
    #GIT_REPOSITORY https://github.com/unum-cloud/picohttpparser.git
    GIT_REPOSITORY https://github.com/MarkReedZ/picohttpparser.git
    GIT_SHALLOW 1
)
FetchContent_MakeAvailable(picohttpparser)
include_directories(${picohttpparser_SOURCE_DIR})

# Base64 decoding
FetchContent_Declare(
    tb64
    GIT_REPOSITORY https://github.com/unum-cloud/Turbo-Base64.git
    GIT_SHALLOW 1
)
FetchContent_MakeAvailable(tb64)
include_directories(${tb64_SOURCE_DIR})
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# LibUring
if(LINUX)
    set(URING_DIR ${CMAKE_BINARY_DIR}/_deps/liburing-ep)
    ExternalProject_Add(
        liburing-ep
        GIT_REPOSITORY https://github.com/axboe/liburing.git
        GIT_TAG liburing-2.3
        GIT_SHALLOW 1
        PREFIX ${CMAKE_BINARY_DIR}/_deps/
        SOURCE_DIR ${URING_DIR}
        CONFIGURE_COMMAND echo Configuring LibUring && cd ${URING_DIR} && ./configure --nolibc --cc=${CMAKE_C_COMPILER} --cxx=${CMAKE_CXX_COMPILER};
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

add_library(ucall_server_posix src/engine_posix.cpp)
target_link_libraries(ucall_server_posix simdjson::simdjson Threads::Threads )
set(PYTHON_BACKEND ucall_server_posix)

add_executable(ucall_example_login_posix examples/login/ucall_server.cpp)
target_link_libraries(ucall_example_login_posix ucall_server_posix cxxopts)
target_compile_options(ucall_example_login_posix PUBLIC -DCXXOPTS_NO_EXCEPTIONS=ON)

if(LINUX)
    add_library(ucall_server_uring src/engine_uring.cpp)
    set(PYTHON_BACKEND ucall_server_uring)
    target_link_libraries(ucall_server_uring simdjson::simdjson Threads::Threads ${URING_LIBS})
    add_executable(ucall_example_login_uring examples/login/ucall_server.cpp)
    target_link_libraries(ucall_example_login_uring ucall_server_uring cxxopts)
    target_compile_options(ucall_example_login_uring PUBLIC -DCXXOPTS_NO_EXCEPTIONS=ON)
endif()

if(UCALL_BUILD_EXAMPLES)
    add_executable(ucall_example_redis examples/redis/ucall_server.cpp)
    target_link_libraries(ucall_example_redis ucall_server_posix)

    find_package(Torch)
    add_executable(ucall_example_pytorcs examples/pytorch/ucall_server.cpp)
    target_link_libraries(ucall_example_pytorcs ucall_server_posix "${TORCH_LIBRARIES}")
endif()

# Python bindings
find_package(Python3 REQUIRED Development.Module)
include_directories(${Python_INCLUDE_DIRS})

if(LINUX)
    Python3_add_library(py_ucall_uring src/python.c)
    target_include_directories(py_ucall_uring PUBLIC src/ include/)
    target_link_libraries(py_ucall_uring PRIVATE ucall_server_uring base64)
    set_target_properties(py_ucall_uring PROPERTIES OUTPUT_NAME uring)
    target_compile_definitions(py_ucall_uring PRIVATE UCALL_PYTHON_MODULE_NAME=uring)
endif()

Python3_add_library(py_ucall_posix src/python.c)
target_include_directories(py_ucall_posix PUBLIC src/ include/)
target_link_libraries(py_ucall_posix PRIVATE ucall_server_posix base64)
set_target_properties(py_ucall_posix PROPERTIES OUTPUT_NAME posix)
target_compile_definitions(py_ucall_posix PRIVATE UCALL_PYTHON_MODULE_NAME=posix)
