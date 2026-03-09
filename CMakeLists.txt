cmake_minimum_required(VERSION 3.16)

project(memrpc LANGUAGES CXX)

CMAKE_POLICY(SET CMP0076 NEW)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          ${CMAKE_BINARY_DIR}/compile_commands.json
          ${CMAKE_SOURCE_DIR}/compile_commands.json)

enable_testing()

find_package(GTest REQUIRED)

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(demo)
