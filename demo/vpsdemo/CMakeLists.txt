cmake_minimum_required(VERSION 3.16)
project(vpsdemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Root of the memrpc repo.
set(MEMRPC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# --- memrpc core library ---
add_library(memrpc_core
    ${MEMRPC_ROOT}/src/client/rpc_client.cpp
    ${MEMRPC_ROOT}/src/server/rpc_server.cpp
    ${MEMRPC_ROOT}/src/bootstrap/posix_demo_bootstrap.cpp
    ${MEMRPC_ROOT}/src/bootstrap/sa_bootstrap.cpp
    ${MEMRPC_ROOT}/src/core/byte_reader.cpp
    ${MEMRPC_ROOT}/src/core/byte_writer.cpp
    ${MEMRPC_ROOT}/src/core/log.cpp
    ${MEMRPC_ROOT}/src/core/slot_pool.cpp
    ${MEMRPC_ROOT}/src/core/session.cpp
)
target_include_directories(memrpc_core PUBLIC
    ${MEMRPC_ROOT}/include
    ${MEMRPC_ROOT}/src
)
target_compile_features(memrpc_core PUBLIC cxx_std_17)
target_link_libraries(memrpc_core PUBLIC pthread rt)

# --- ohos_sa_mock library ---
add_library(ohos_sa_mock
    ${MEMRPC_ROOT}/third_party/ohos_sa_mock/src/system_ability_manager.cpp
    ${MEMRPC_ROOT}/third_party/ohos_sa_mock/src/iremote_object.cpp
    ${MEMRPC_ROOT}/third_party/ohos_sa_mock/src/iremote_broker_registry.cpp
)
target_include_directories(ohos_sa_mock PUBLIC
    ${MEMRPC_ROOT}/third_party/ohos_sa_mock/include
)
target_compile_features(ohos_sa_mock PUBLIC cxx_std_17)

# --- vpsdemo library (registry + bootstrap + SCM_RIGHTS + demo service) ---
add_library(vpsdemo_lib
    src/registry_protocol.cpp
    src/registry_server.cpp
    src/registry_client.cpp
    src/registry_backend.cpp
    src/scm_rights.cpp
    src/vps_bootstrap_stub.cpp
    src/vps_bootstrap_proxy.cpp
    src/vps_client.cpp
    src/vpsdemo_service.cpp
)
target_include_directories(vpsdemo_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vpsdemo_lib PUBLIC memrpc_core ohos_sa_mock)

# --- Executables ---
add_executable(vpsdemo_supervisor src/vpsdemo_supervisor.cpp)
target_link_libraries(vpsdemo_supervisor PRIVATE vpsdemo_lib)

add_executable(vpsdemo_engine_sa src/vpsdemo_engine_sa.cpp)
target_link_libraries(vpsdemo_engine_sa PRIVATE vpsdemo_lib)

add_executable(vpsdemo_client src/vpsdemo_client.cpp)
target_link_libraries(vpsdemo_client PRIVATE vpsdemo_lib)
