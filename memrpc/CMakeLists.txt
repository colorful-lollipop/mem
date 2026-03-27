set(MEMRPC_MODULE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(src)

add_library(memrpc_test_support
  tests/support/dev_bootstrap.cpp
)
target_include_directories(memrpc_test_support
  PUBLIC
    ${MEMRPC_MODULE_ROOT}/include
    ${MEMRPC_MODULE_ROOT}/tests/support/include
  PRIVATE
    ${MEMRPC_MODULE_ROOT}/src
    ${PROJECT_SOURCE_DIR}/include
)
target_compile_features(memrpc_test_support PUBLIC cxx_std_17)
target_link_libraries(memrpc_test_support PUBLIC memrpc_core)

add_subdirectory(tests)
