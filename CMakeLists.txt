cmake_minimum_required(VERSION 3.20)
project(cstp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cstp_protocol
  src/protocol.cpp
  src/socket_io.cpp
)

target_include_directories(cstp_protocol
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_options(cstp_protocol
  PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

add_executable(cstp_test
  tests/test_protocol.cpp
)
target_link_libraries(cstp_test PRIVATE cstp_protocol)

enable_testing()
add_test(NAME cstp_test COMMAND cstp_test)

add_executable(cstp_server
  examples/server.cpp
)
target_link_libraries(cstp_server PRIVATE cstp_protocol)

add_executable(cstp_client
  examples/client.cpp
)
target_link_libraries(cstp_client PRIVATE cstp_protocol)
