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
    ${MEMRPC_ROOT}/third_party/ohos_sa_mock/src/scm_rights.cpp
    ${MEMRPC_ROOT}/third_party/ohos_sa_mock/src/mock_service_socket.cpp
)
target_include_directories(ohos_sa_mock PUBLIC
    ${MEMRPC_ROOT}/third_party/ohos_sa_mock/include
)
target_compile_features(ohos_sa_mock PUBLIC cxx_std_17)

# --- vpsdemo library (registry + bootstrap + demo service) ---
add_library(vpsdemo_lib
    src/registry_protocol.cpp
    src/registry_server.cpp
    src/registry_client.cpp
    src/registry_backend.cpp
    src/virus_executor_service.cpp
    src/ves_bootstrap_proxy.cpp
    src/ves_client.cpp
    src/ves_session_service.cpp
    src/vesdemo_sample_rules.cpp
    src/ves_engine_service.cpp
)
target_include_directories(vpsdemo_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(vpsdemo_lib PUBLIC memrpc_core ohos_sa_mock)

# --- Executables ---
add_executable(vpsdemo_supervisor src/vesdemo_supervisor.cpp)
target_link_libraries(vpsdemo_supervisor PRIVATE vpsdemo_lib)

add_executable(vpsdemo_engine_sa src/vesdemo_engine_sa.cpp)
target_link_libraries(vpsdemo_engine_sa PRIVATE vpsdemo_lib)

add_executable(vpsdemo_client src/vesdemo_client.cpp)
target_link_libraries(vpsdemo_client PRIVATE vpsdemo_lib)

add_executable(vpsdemo_stress_client src/vesdemo_stress_client.cpp)
target_link_libraries(vpsdemo_stress_client PRIVATE vpsdemo_lib)

add_executable(vpsdemo_dt_crash_recovery src/vesdemo_dt_crash_recovery.cpp)
target_link_libraries(vpsdemo_dt_crash_recovery PRIVATE vpsdemo_lib)

option(VPSDEMO_ENABLE_TESTS "Build vpsdemo tests" OFF)
option(VPSDEMO_ENABLE_FUZZ "Build vpsdemo fuzz targets" OFF)

if (VPSDEMO_ENABLE_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(vpsdemo_session_service_test tests/ves_session_service_test.cpp)
  target_link_libraries(vpsdemo_session_service_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_session_service_test COMMAND vpsdemo_session_service_test)

  add_executable(vpsdemo_heartbeat_test tests/ves_heartbeat_test.cpp)
  target_link_libraries(vpsdemo_heartbeat_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_heartbeat_test COMMAND vpsdemo_heartbeat_test)

  add_executable(vpsdemo_codec_test tests/vesdemo_codec_test.cpp)
  target_link_libraries(vpsdemo_codec_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_codec_test COMMAND vpsdemo_codec_test)

  add_executable(vpsdemo_sample_rules_test tests/vesdemo_sample_rules_test.cpp)
  target_link_libraries(vpsdemo_sample_rules_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_sample_rules_test COMMAND vpsdemo_sample_rules_test)

  add_executable(vpsdemo_health_test tests/vesdemo_health_test.cpp)
  target_link_libraries(vpsdemo_health_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_health_test COMMAND vpsdemo_health_test)

  add_executable(vpsdemo_policy_test tests/vesdemo_policy_test.cpp)
  target_link_libraries(vpsdemo_policy_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_policy_test COMMAND vpsdemo_policy_test)

  add_executable(vpsdemo_crash_recovery_test tests/vesdemo_crash_recovery_test.cpp)
  target_link_libraries(vpsdemo_crash_recovery_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_crash_recovery_test COMMAND vpsdemo_crash_recovery_test)
  set_tests_properties(vpsdemo_crash_recovery_test PROPERTIES LABELS "dt")

  # Integration tests (executables that are themselves tests)
  add_test(NAME vpsdemo_supervisor_integration_test 
           COMMAND vpsdemo_supervisor
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(vpsdemo_supervisor_integration_test PROPERTIES
                       LABELS "integration"
                       TIMEOUT 30)

  add_test(NAME vpsdemo_stress_test 
           COMMAND vpsdemo_stress_client --threads 2 --iterations 100 --seed 42 --no-crash
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(vpsdemo_stress_test PROPERTIES
                       LABELS "stress"
                       TIMEOUT 30)

  add_test(NAME vpsdemo_dt_crash_recovery_test 
           COMMAND vpsdemo_dt_crash_recovery
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(vpsdemo_dt_crash_recovery_test PROPERTIES
                       LABELS "dt"
                       TIMEOUT 60)

  if (VPSDEMO_ENABLE_FUZZ)
    add_subdirectory(tests/fuzz)
  endif()
endif()
