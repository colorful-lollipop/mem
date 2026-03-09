add_library(memrpc
  client/engine_client.cpp
  server/engine_server.cpp
  bootstrap/posix_demo_bootstrap.cpp
  bootstrap/sa_bootstrap.cpp
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
