cmake_minimum_required(VERSION 3.16)

project(memrpc LANGUAGES CXX)

CMAKE_POLICY(SET CMP0076 NEW)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
execute_process(
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          ${CMAKE_BINARY_DIR}/compile_commands.json
          ${CMAKE_SOURCE_DIR}/compile_commands.json)

option(MEMRPC_ENABLE_STRESS_TESTS "Enable stress test targets" OFF)
option(MEMRPC_ENABLE_FUZZ "Enable fuzz targets" OFF)

enable_testing()

find_package(GTest REQUIRED)

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(demo)
