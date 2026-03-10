add_library(memrpc
  client/rpc_client.cpp
  server/rpc_server.cpp
  bootstrap/posix_demo_bootstrap.cpp
  bootstrap/sa_bootstrap.cpp
  core/byte_reader.cpp
  core/byte_writer.cpp
  core/log.cpp
  core/ring_buffer.cpp
  core/slot_pool.cpp
  core/session.cpp
)

target_include_directories(memrpc
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_compile_features(memrpc PUBLIC cxx_std_17)

add_library(minirpc_demo
  apps/minirpc/common/minirpc_codec.cpp
  apps/minirpc/child/minirpc_service.cpp
  apps/minirpc/parent/minirpc_async_client.cpp
  apps/minirpc/parent/minirpc_client.cpp
  apps/minirpc/parent/minirpc_resilient_invoker.cpp
)

target_include_directories(minirpc_demo
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(minirpc_demo PUBLIC memrpc)
target_compile_features(minirpc_demo PUBLIC cxx_std_17)
