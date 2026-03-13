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
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_features(memrpc_core PUBLIC cxx_std_17)
target_link_libraries(memrpc_core PUBLIC pthread rt)

add_library(memrpc ALIAS memrpc_core)
