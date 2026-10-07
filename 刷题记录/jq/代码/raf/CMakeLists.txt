cmake_minimum_required(VERSION 3.16)
project(raf LANGUAGES CXX)

# C++23 where the compiler has it, for std::string::resize_and_overwrite:
# sizing a read buffer with resize() zeroes it first, which costs 15 us on
# every 1 MiB record raf reads and is thrown away by the read that follows.
# The code falls back to resize() under C++20, so 20 still builds.
if(NOT DEFINED CMAKE_CXX_STANDARD)
  if(cxx_std_23 IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    set(CMAKE_CXX_STANDARD 23)
  else()
    set(CMAKE_CXX_STANDARD 20)
  endif()
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(RAF_URING "Build the io_uring backend (needs liburing)" OFF)
option(RAF_BUILD_TESTS "Build raf tests" ON)
option(RAF_BUILD_BENCH "Build raf benchmark" ON)
option(RAF_SANITIZE "Build with address/undefined sanitizers" OFF)
option(RAF_TSAN "Build with the thread sanitizer" OFF)
set(RAF_LOG_LEVEL "info" CACHE STRING
  "Lowest log level compiled in: trace, debug, info, warn, error, off")

find_package(Threads REQUIRED)

# spdlog is the one hard dependency of the library. Distro packages are
# preferred; when there is none, fetch it rather than failing, so a machine
# borrowed for a benchmark run does not need its package manager touched.
# -DRAF_FETCH_DEPS=OFF turns the fallback off and demands a local copy.
option(RAF_FETCH_DEPS "Download missing dependencies instead of failing" ON)
find_package(spdlog CONFIG QUIET)
if(NOT spdlog_FOUND)
  if(NOT RAF_FETCH_DEPS)
    message(FATAL_ERROR
      "spdlog not found. Install it (apt install libspdlog-dev, dnf install "
      "spdlog-devel, brew install spdlog) or allow -DRAF_FETCH_DEPS=ON.")
  endif()
  message(STATUS "spdlog not found; fetching it (set -DRAF_FETCH_DEPS=OFF to require a local copy)")
  include(FetchContent)
  set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.14.1
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(spdlog)
endif()

add_library(raf
  src/common/crc32c.cpp
  src/common/log.cpp
  src/io/posix_backend.cpp
  src/store/read_window.cpp
  src/store/segment.cpp
  src/store/archive.cpp
  src/api/index.cpp
  src/api/writer.cpp
  src/api/reader.cpp
)
target_include_directories(raf
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(raf PUBLIC Threads::Threads PRIVATE spdlog::spdlog)

# The io_uring backend is optional: liburing is not everywhere, and the POSIX
# backend is what the default build uses either way.
if(RAF_URING)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
  target_sources(raf PRIVATE src/io/uring_backend.cpp)
  target_link_libraries(raf PRIVATE PkgConfig::LIBURING)
  target_compile_definitions(raf PUBLIC RAF_HAS_URING)
endif()

# Compile the quiet levels out entirely so the append path does not even carry
# their string literals.
string(TOUPPER "${RAF_LOG_LEVEL}" _raf_level)
set(_raf_levels TRACE DEBUG INFO WARN ERROR OFF)
if(NOT _raf_level IN_LIST _raf_levels)
  message(FATAL_ERROR "RAF_LOG_LEVEL must be one of: ${_raf_levels}")
endif()
target_compile_definitions(raf PRIVATE SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_${_raf_level})
target_compile_options(raf PRIVATE -Wall -Wextra -Wpedantic)

if(RAF_SANITIZE)
  target_compile_options(raf PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(raf PUBLIC -fsanitize=address,undefined)
endif()
if(RAF_TSAN)
  target_compile_options(raf PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(raf PUBLIC -fsanitize=thread)
endif()

if(RAF_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    if(NOT RAF_FETCH_DEPS)
      message(FATAL_ERROR "GoogleTest not found; install it or build with -DRAF_BUILD_TESTS=OFF.")
    endif()
    message(STATUS "GoogleTest not found; fetching it")
    include(FetchContent)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.14.0
      GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(raf_test
    tests/main.cpp
    tests/test_crc32c.cpp
    tests/test_format.cpp
    tests/test_backend.cpp
    tests/test_faults.cpp
    tests/test_segment.cpp
    tests/test_api.cpp
    tests/test_boundary.cpp
    tests/test_errors.cpp
    tests/test_recovery.cpp
    tests/test_iteration.cpp
    tests/test_direct.cpp
    tests/test_concurrency.cpp
    tests/test_multiprocess.cpp
    tests/test_stress.cpp
  )
  target_include_directories(raf_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(raf_test PRIVATE raf GTest::gtest spdlog::spdlog)
  target_compile_options(raf_test PRIVATE -Wall -Wextra)
  add_test(NAME raf_test COMMAND raf_test)
  set_tests_properties(raf_test PROPERTIES TIMEOUT 900)

  # The short suite: the properties raf would be broken without, and nothing
  # else. See tests/core/README.md for what is left out and why.
  add_executable(raf_core_test
    tests/main.cpp
    tests/core/test_core.cpp
  )
  target_include_directories(raf_core_test
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(raf_core_test PRIVATE raf GTest::gtest spdlog::spdlog)
  target_compile_options(raf_core_test PRIVATE -Wall -Wextra)
  add_test(NAME raf_core_test COMMAND raf_core_test)
  set_tests_properties(raf_core_test PROPERTIES TIMEOUT 300)
endif()

if(RAF_BUILD_BENCH)
  add_executable(raf_bench bench/bench_raf.cpp)
  target_include_directories(raf_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(raf_bench PRIVATE raf)
  target_compile_options(raf_bench PRIVATE -Wall -Wextra)

  # Driven by the scripts in bench/physical; reports latency percentiles and
  # device-level read amplification, which raf_bench does not.
  add_executable(raf_matrix bench/raf_matrix.cpp)
  # The O_DIRECT read mode parses segments itself, so it needs the on-disk
  # format. It is a measurement, not a second implementation: nothing else in
  # the benchmark reaches past the public header.
  target_include_directories(raf_matrix PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(raf_matrix PRIVATE raf)
  target_compile_options(raf_matrix PRIVATE -Wall -Wextra)
endif()
