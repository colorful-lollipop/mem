cmake_minimum_required(VERSION 3.16)
project(vpsdemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Root of the memrpc repo.
set(MEMRPC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(MEMRPC_MODULE_ROOT ${MEMRPC_ROOT}/memrpc)

if (NOT TARGET memrpc_core)
  add_library(memrpc_core
      ${MEMRPC_MODULE_ROOT}/src/client/rpc_client.cpp
      ${MEMRPC_MODULE_ROOT}/src/server/rpc_server.cpp
      ${MEMRPC_MODULE_ROOT}/src/bootstrap/posix_demo_bootstrap.cpp
      ${MEMRPC_MODULE_ROOT}/src/bootstrap/sa_bootstrap.cpp
      ${MEMRPC_MODULE_ROOT}/src/core/byte_reader.cpp
      ${MEMRPC_MODULE_ROOT}/src/core/byte_writer.cpp
      ${MEMRPC_MODULE_ROOT}/src/core/log.cpp
      ${MEMRPC_MODULE_ROOT}/src/core/slot_pool.cpp
      ${MEMRPC_MODULE_ROOT}/src/core/session.cpp
  )
  target_include_directories(memrpc_core PUBLIC
      ${MEMRPC_ROOT}/include
      ${MEMRPC_MODULE_ROOT}/include
      ${MEMRPC_MODULE_ROOT}/src
  )
  target_compile_features(memrpc_core PUBLIC cxx_std_17)
  target_link_libraries(memrpc_core PUBLIC pthread rt)
endif()

if (NOT TARGET memrpc)
  add_library(memrpc ALIAS memrpc_core)
endif()

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
    src/testkit_service.cpp
    src/testkit_async_client.cpp
    src/testkit_client.cpp
)
target_include_directories(vpsdemo_lib PUBLIC
    ${MEMRPC_ROOT}/include
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

  add_executable(vpsdemo_testkit_headers_test tests/testkit_headers_test.cpp)
  target_link_libraries(vpsdemo_testkit_headers_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_headers_test COMMAND vpsdemo_testkit_headers_test)

  add_executable(vpsdemo_testkit_codec_test tests/testkit_codec_test.cpp)
  target_link_libraries(vpsdemo_testkit_codec_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_codec_test COMMAND vpsdemo_testkit_codec_test)

  add_executable(vpsdemo_testkit_service_test tests/testkit_service_test.cpp)
  target_link_libraries(vpsdemo_testkit_service_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_service_test COMMAND vpsdemo_testkit_service_test)

  add_executable(vpsdemo_testkit_baseline_test tests/testkit_baseline_test.cpp)
  target_link_libraries(vpsdemo_testkit_baseline_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_baseline_test COMMAND vpsdemo_testkit_baseline_test)

  add_executable(vpsdemo_testkit_client_test tests/testkit_client_test.cpp)
  target_link_libraries(vpsdemo_testkit_client_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_client_test COMMAND vpsdemo_testkit_client_test)

  add_executable(vpsdemo_testkit_dfx_test
    tests/testkit_dfx_test.cpp
    tests/testkit_resilient_invoker.cpp
  )
  target_link_libraries(vpsdemo_testkit_dfx_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_dfx_test COMMAND vpsdemo_testkit_dfx_test)

  add_executable(vpsdemo_testkit_backpressure_test tests/testkit_backpressure_test.cpp)
  target_link_libraries(vpsdemo_testkit_backpressure_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_backpressure_test COMMAND vpsdemo_testkit_backpressure_test)

  add_executable(vpsdemo_testkit_async_pipeline_test tests/testkit_async_pipeline_test.cpp)
  target_link_libraries(vpsdemo_testkit_async_pipeline_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_async_pipeline_test COMMAND vpsdemo_testkit_async_pipeline_test)

  add_executable(vpsdemo_testkit_latency_test tests/testkit_latency_test.cpp)
  target_link_libraries(vpsdemo_testkit_latency_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_latency_test COMMAND vpsdemo_testkit_latency_test)

  add_executable(vpsdemo_testkit_throughput_test tests/testkit_throughput_test.cpp)
  target_link_libraries(vpsdemo_testkit_throughput_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_throughput_test COMMAND vpsdemo_testkit_throughput_test)
  set_tests_properties(vpsdemo_testkit_throughput_test PROPERTIES
    ENVIRONMENT
      "MEMRPC_PERF_BASELINE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/perf_baselines/testkit_throughput.baseline"
  )

  add_executable(vpsdemo_testkit_dt_stability_test tests/testkit_dt_stability_test.cpp)
  target_link_libraries(vpsdemo_testkit_dt_stability_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_dt_stability_test COMMAND vpsdemo_testkit_dt_stability_test)
  set_tests_properties(vpsdemo_testkit_dt_stability_test PROPERTIES LABELS "dt_stability")

  add_executable(vpsdemo_testkit_dt_perf_test tests/testkit_dt_perf_test.cpp)
  target_link_libraries(vpsdemo_testkit_dt_perf_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_dt_perf_test COMMAND vpsdemo_testkit_dt_perf_test)
  set_tests_properties(vpsdemo_testkit_dt_perf_test PROPERTIES
    LABELS "dt_perf"
    ENVIRONMENT
      "MEMRPC_DT_BASELINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/perf_baselines"
  )

  add_library(vpsdemo_testkit_stress_config tests/testkit_stress_config.cpp)
  target_link_libraries(vpsdemo_testkit_stress_config PUBLIC vpsdemo_lib)
  target_compile_features(vpsdemo_testkit_stress_config PUBLIC cxx_std_17)

  add_executable(vpsdemo_testkit_stress_config_test tests/testkit_stress_config_test.cpp)
  target_link_libraries(
    vpsdemo_testkit_stress_config_test PRIVATE vpsdemo_testkit_stress_config GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_stress_config_test COMMAND vpsdemo_testkit_stress_config_test)
  set_tests_properties(vpsdemo_testkit_stress_config_test PROPERTIES LABELS "stress")

  add_executable(vpsdemo_testkit_stress_runner tests/testkit_stress_runner.cpp)
  target_link_libraries(vpsdemo_testkit_stress_runner PRIVATE vpsdemo_testkit_stress_config vpsdemo_lib)
  target_compile_features(vpsdemo_testkit_stress_runner PRIVATE cxx_std_17)

  add_test(NAME vpsdemo_testkit_stress_smoke COMMAND vpsdemo_testkit_stress_runner)
  set_tests_properties(vpsdemo_testkit_stress_smoke PROPERTIES
    LABELS "stress"
    ENVIRONMENT
      "MEMRPC_STRESS_DURATION_SEC=1;MEMRPC_STRESS_WARMUP_SEC=0;MEMRPC_STRESS_THREADS=1;MEMRPC_STRESS_ECHO_WEIGHT=100;MEMRPC_STRESS_ADD_WEIGHT=0;MEMRPC_STRESS_SLEEP_WEIGHT=0;MEMRPC_STRESS_PAYLOAD_SIZES=0,16;MEMRPC_STRESS_MAX_SLEEP_MS=1;MEMRPC_STRESS_SEED=1"
  )

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
