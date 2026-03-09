cmake_minimum_required(VERSION 3.16)

project(memrpc LANGUAGES CXX)

enable_testing()

find_package(GTest REQUIRED)

add_subdirectory(src)
add_subdirectory(tests)
add_subdirectory(demo)
