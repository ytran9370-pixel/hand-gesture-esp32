# PC host library and tools for Hand Gesture ESP32.
#
#   cmake -S pc_app -B build/pc_app -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build/pc_app -j
#   ctest --test-dir build/pc_app --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(hg_pc_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

option(HG_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" ON)
option(HG_BUILD_TESTS "Build the GoogleTest unit tests" ON)

if(MSVC)
  set(HG_WARNINGS /W4 /WX)
else()
  set(HG_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Werror)
endif()

if(HG_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---- library: protocol, parser, serial ports, client ----
set(HG_SOURCES
  src/protocol.cpp
  src/frame_parser.cpp
  src/messages.cpp
  src/mock_serial_port.cpp
  src/serial_port_factory.cpp
  src/gesture_client.cpp)
if(UNIX)
  list(APPEND HG_SOURCES src/posix_serial_port.cpp)
endif()

add_library(hg_host STATIC ${HG_SOURCES})
target_include_directories(hg_host PUBLIC include)
target_compile_options(hg_host PRIVATE ${HG_WARNINGS})

# ---- command-line tool ----
add_executable(hgcli apps/hgcli.cpp)
target_link_libraries(hgcli PRIVATE hg_host)
target_compile_options(hgcli PRIVATE ${HG_WARNINGS})

# ---- unit tests ----
if(HG_BUILD_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
  add_executable(hg_tests
    tests/test_protocol.cpp
    tests/test_frame_parser.cpp
    tests/test_gesture_client.cpp)
  target_link_libraries(hg_tests PRIVATE hg_host GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(hg_tests)
endif()
