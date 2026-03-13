cmake_minimum_required(VERSION 3.16)

# 强制使用 Clang 工具链
set(CMAKE_C_COMPILER clang CACHE STRING "C compiler" FORCE)
set(CMAKE_CXX_COMPILER clang++ CACHE STRING "C++ compiler" FORCE)

project(virus_executor_service LANGUAGES CXX)

CMAKE_POLICY(SET CMP0076 NEW)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(${CMAKE_SOURCE_DIR}/cmake/ProjectOptions.cmake)
memrpc_configure_project_options()
execute_process(
  COMMAND ${CMAKE_COMMAND} -E create_symlink
          ${CMAKE_BINARY_DIR}/compile_commands.json
          ${CMAKE_SOURCE_DIR}/compile_commands.json)

option(MEMRPC_ENABLE_STRESS_TESTS "Enable stress test targets" OFF)
option(MEMRPC_ENABLE_FUZZ "Enable fuzz targets" OFF)
option(MEMRPC_ENABLE_DT_TESTS "Enable DT stability/perf tests" ON)

enable_testing()

find_package(GTest REQUIRED)

# memrpc is a reusable dependency module, while the repository mainline is the
# virus_executor_service application and its test surface.
add_subdirectory(memrpc)
add_subdirectory(virus_executor_service)
