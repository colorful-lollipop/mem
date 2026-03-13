add_library(memrpc_core
  client/rpc_client.cpp
  server/rpc_server.cpp
  bootstrap/posix_demo_bootstrap.cpp
  bootstrap/sa_bootstrap.cpp
  core/byte_reader.cpp
  core/byte_writer.cpp
  core/log.cpp
  core/slot_pool.cpp
  core/session.cpp
)

target_include_directories(memrpc_core
  PUBLIC
    ${MEMRPC_MODULE_ROOT}/include
  PRIVATE
    ${MEMRPC_MODULE_ROOT}/src
    ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(memrpc_core PUBLIC cxx_std_17)
target_link_libraries(memrpc_core PUBLIC pthread rt)

add_library(memrpc ALIAS memrpc_core)
