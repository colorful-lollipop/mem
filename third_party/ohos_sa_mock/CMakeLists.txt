cmake_minimum_required(VERSION 3.16)

project(ohos_sa_mock LANGUAGES CXX)

option(OHOS_SA_MOCK_BUILD_TESTS "Build ohos_sa_mock tests" ON)
option(OHOS_SA_MOCK_BUILD_DEMO "Build ohos_sa_mock demo" ON)

if (CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  enable_testing()
  find_package(GTest REQUIRED)
endif()

add_library(ohos_sa_mock
  src/iremote_object.cpp
  src/iremote_broker_registry.cpp
  src/system_ability_manager.cpp
)

target_include_directories(ohos_sa_mock
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(ohos_sa_mock PUBLIC cxx_std_17)

if (OHOS_SA_MOCK_BUILD_DEMO)
  add_executable(ohos_sa_mock_demo demo/sa_mock_demo.cpp)
  target_link_libraries(ohos_sa_mock_demo PRIVATE ohos_sa_mock)
endif()

if (OHOS_SA_MOCK_BUILD_TESTS)
  add_subdirectory(tests)
endif()
