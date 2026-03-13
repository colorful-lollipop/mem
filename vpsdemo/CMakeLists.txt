cmake_minimum_required(VERSION 3.16)
project(vpsdemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Root of the repo.
set(MEMRPC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
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
      ${MEMRPC_MODULE_ROOT}/include
      ${MEMRPC_MODULE_ROOT}/src
  )
  target_include_directories(memrpc_core PRIVATE
      ${MEMRPC_ROOT}/include
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

set(VPSDEMO_PUBLIC_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MEMRPC_ROOT}/include
)

add_library(vpsdemo_transport
    src/transport/registry_protocol.cpp
    src/transport/registry_server.cpp
    src/transport/registry_client.cpp
    src/transport/registry_backend.cpp
    src/transport/ves_bootstrap_proxy.cpp
)
target_include_directories(vpsdemo_transport PUBLIC ${VPSDEMO_PUBLIC_INCLUDE_DIRS})
target_link_libraries(vpsdemo_transport PUBLIC memrpc_core ohos_sa_mock)
target_compile_features(vpsdemo_transport PUBLIC cxx_std_17)

add_library(vpsdemo_ves
    src/ves/vesdemo_sample_rules.cpp
)
target_include_directories(vpsdemo_ves PUBLIC ${VPSDEMO_PUBLIC_INCLUDE_DIRS})
target_compile_features(vpsdemo_ves PUBLIC cxx_std_17)

add_library(vpsdemo_client_lib
    src/client/ves_client.cpp
)
target_include_directories(vpsdemo_client_lib PUBLIC ${VPSDEMO_PUBLIC_INCLUDE_DIRS})
target_link_libraries(vpsdemo_client_lib PUBLIC memrpc_core vpsdemo_transport)
target_compile_features(vpsdemo_client_lib PUBLIC cxx_std_17)

add_library(vpsdemo_testkit
    src/testkit/testkit_service.cpp
    src/testkit/testkit_async_client.cpp
    src/testkit/testkit_client.cpp
)
target_include_directories(vpsdemo_testkit PUBLIC ${VPSDEMO_PUBLIC_INCLUDE_DIRS})
target_link_libraries(vpsdemo_testkit PUBLIC memrpc_core)
target_compile_features(vpsdemo_testkit PUBLIC cxx_std_17)

add_library(vpsdemo_service
    src/service/virus_executor_service.cpp
    src/service/ves_session_service.cpp
    src/service/ves_engine_service.cpp
)
target_include_directories(vpsdemo_service PUBLIC ${VPSDEMO_PUBLIC_INCLUDE_DIRS})
target_link_libraries(vpsdemo_service PUBLIC memrpc_core vpsdemo_ves vpsdemo_testkit ohos_sa_mock)
target_compile_features(vpsdemo_service PUBLIC cxx_std_17)

# Aggregate application surface for tests and simple entrypoints.
add_library(vpsdemo_lib INTERFACE)
target_include_directories(vpsdemo_lib INTERFACE ${VPSDEMO_PUBLIC_INCLUDE_DIRS})
target_link_libraries(vpsdemo_lib INTERFACE
    vpsdemo_transport
    vpsdemo_ves
    vpsdemo_client_lib
    vpsdemo_testkit
    vpsdemo_service
)

# --- Executables ---
add_executable(vpsdemo_supervisor src/app/vesdemo_supervisor.cpp)
target_link_libraries(vpsdemo_supervisor PRIVATE vpsdemo_transport)

add_executable(vpsdemo_engine_sa src/app/vesdemo_engine_sa.cpp)
target_link_libraries(vpsdemo_engine_sa PRIVATE vpsdemo_transport vpsdemo_service)

add_executable(vpsdemo_client src/app/vesdemo_client.cpp)
target_link_libraries(vpsdemo_client PRIVATE vpsdemo_transport vpsdemo_client_lib)

add_executable(vpsdemo_stress_client src/app/vesdemo_stress_client.cpp)
target_link_libraries(vpsdemo_stress_client PRIVATE
    vpsdemo_transport
    vpsdemo_client_lib
    vpsdemo_ves
)

add_executable(vpsdemo_dt_crash_recovery src/app/vesdemo_dt_crash_recovery.cpp)
target_link_libraries(vpsdemo_dt_crash_recovery PRIVATE
    vpsdemo_transport
    vpsdemo_client_lib
)

option(VPSDEMO_ENABLE_TESTS "Build vpsdemo tests" OFF)
option(VPSDEMO_ENABLE_FUZZ "Build vpsdemo fuzz targets" OFF)

if (VPSDEMO_ENABLE_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(vpsdemo_session_service_test tests/unit/ves/ves_session_service_test.cpp)
  target_link_libraries(vpsdemo_session_service_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_session_service_test COMMAND vpsdemo_session_service_test)

  add_executable(vpsdemo_heartbeat_test tests/unit/ves/ves_heartbeat_test.cpp)
  target_link_libraries(vpsdemo_heartbeat_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_heartbeat_test COMMAND vpsdemo_heartbeat_test)

  add_executable(vpsdemo_codec_test tests/unit/ves/vesdemo_codec_test.cpp)
  target_link_libraries(vpsdemo_codec_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_codec_test COMMAND vpsdemo_codec_test)

  add_executable(vpsdemo_testkit_headers_test tests/unit/testkit/testkit_headers_test.cpp)
  target_link_libraries(vpsdemo_testkit_headers_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_headers_test COMMAND vpsdemo_testkit_headers_test)

  add_executable(vpsdemo_testkit_codec_test tests/unit/testkit/testkit_codec_test.cpp)
  target_link_libraries(vpsdemo_testkit_codec_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_codec_test COMMAND vpsdemo_testkit_codec_test)

  add_executable(vpsdemo_testkit_service_test tests/unit/testkit/testkit_service_test.cpp)
  target_link_libraries(vpsdemo_testkit_service_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_service_test COMMAND vpsdemo_testkit_service_test)

  add_executable(vpsdemo_testkit_baseline_test tests/unit/testkit/testkit_baseline_test.cpp)
  target_link_libraries(vpsdemo_testkit_baseline_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_baseline_test COMMAND vpsdemo_testkit_baseline_test)

  add_executable(vpsdemo_testkit_client_test tests/unit/testkit/testkit_client_test.cpp)
  target_link_libraries(vpsdemo_testkit_client_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_client_test COMMAND vpsdemo_testkit_client_test)

  add_executable(vpsdemo_testkit_dfx_test
    tests/unit/testkit/testkit_dfx_test.cpp
    tests/unit/testkit/testkit_resilient_invoker.cpp
  )
  target_link_libraries(vpsdemo_testkit_dfx_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_dfx_test COMMAND vpsdemo_testkit_dfx_test)

  add_executable(vpsdemo_testkit_backpressure_test tests/unit/testkit/testkit_backpressure_test.cpp)
  target_link_libraries(vpsdemo_testkit_backpressure_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_backpressure_test COMMAND vpsdemo_testkit_backpressure_test)

  add_executable(vpsdemo_testkit_async_pipeline_test tests/unit/testkit/testkit_async_pipeline_test.cpp)
  target_link_libraries(vpsdemo_testkit_async_pipeline_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_async_pipeline_test COMMAND vpsdemo_testkit_async_pipeline_test)

  add_executable(vpsdemo_testkit_latency_test tests/unit/testkit/testkit_latency_test.cpp)
  target_link_libraries(vpsdemo_testkit_latency_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_latency_test COMMAND vpsdemo_testkit_latency_test)

  add_executable(vpsdemo_testkit_throughput_test tests/stress/testkit_throughput_test.cpp)
  target_link_libraries(vpsdemo_testkit_throughput_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_throughput_test COMMAND vpsdemo_testkit_throughput_test)
  set_tests_properties(vpsdemo_testkit_throughput_test PROPERTIES
    ENVIRONMENT
      "MEMRPC_PERF_BASELINE_PATH=${CMAKE_CURRENT_SOURCE_DIR}/perf_baselines/testkit_throughput.baseline"
  )

  add_executable(vpsdemo_testkit_dt_stability_test tests/dt/testkit_dt_stability_test.cpp)
  target_link_libraries(vpsdemo_testkit_dt_stability_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_dt_stability_test COMMAND vpsdemo_testkit_dt_stability_test)
  set_tests_properties(vpsdemo_testkit_dt_stability_test PROPERTIES LABELS "dt_stability")

  add_executable(vpsdemo_testkit_dt_perf_test tests/dt/testkit_dt_perf_test.cpp)
  target_link_libraries(vpsdemo_testkit_dt_perf_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_dt_perf_test COMMAND vpsdemo_testkit_dt_perf_test)
  set_tests_properties(vpsdemo_testkit_dt_perf_test PROPERTIES
    LABELS "dt_perf"
    ENVIRONMENT
      "MEMRPC_DT_BASELINE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/perf_baselines"
  )

  add_library(vpsdemo_testkit_stress_config tests/stress/testkit_stress_config.cpp)
  target_link_libraries(vpsdemo_testkit_stress_config PUBLIC vpsdemo_lib)
  target_compile_features(vpsdemo_testkit_stress_config PUBLIC cxx_std_17)

  add_executable(vpsdemo_testkit_stress_config_test tests/stress/testkit_stress_config_test.cpp)
  target_link_libraries(
    vpsdemo_testkit_stress_config_test PRIVATE vpsdemo_testkit_stress_config GTest::gtest_main)
  add_test(NAME vpsdemo_testkit_stress_config_test COMMAND vpsdemo_testkit_stress_config_test)
  set_tests_properties(vpsdemo_testkit_stress_config_test PROPERTIES LABELS "stress")

  add_executable(vpsdemo_testkit_stress_runner tests/stress/testkit_stress_runner.cpp)
  target_link_libraries(vpsdemo_testkit_stress_runner PRIVATE vpsdemo_testkit_stress_config vpsdemo_lib)
  target_compile_features(vpsdemo_testkit_stress_runner PRIVATE cxx_std_17)

  add_test(NAME vpsdemo_testkit_stress_smoke COMMAND vpsdemo_testkit_stress_runner)
  set_tests_properties(vpsdemo_testkit_stress_smoke PROPERTIES
    LABELS "stress"
    ENVIRONMENT
      "MEMRPC_STRESS_DURATION_SEC=1;MEMRPC_STRESS_WARMUP_SEC=0;MEMRPC_STRESS_THREADS=1;MEMRPC_STRESS_ECHO_WEIGHT=100;MEMRPC_STRESS_ADD_WEIGHT=0;MEMRPC_STRESS_SLEEP_WEIGHT=0;MEMRPC_STRESS_PAYLOAD_SIZES=0,16;MEMRPC_STRESS_MAX_SLEEP_MS=1;MEMRPC_STRESS_SEED=1"
  )

  add_executable(vpsdemo_sample_rules_test tests/unit/ves/vesdemo_sample_rules_test.cpp)
  target_link_libraries(vpsdemo_sample_rules_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_sample_rules_test COMMAND vpsdemo_sample_rules_test)

  add_executable(vpsdemo_health_test tests/unit/ves/vesdemo_health_test.cpp)
  target_link_libraries(vpsdemo_health_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_health_test COMMAND vpsdemo_health_test)

  add_executable(vpsdemo_policy_test tests/unit/ves/vesdemo_policy_test.cpp)
  target_link_libraries(vpsdemo_policy_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_policy_test COMMAND vpsdemo_policy_test)

  add_executable(vpsdemo_crash_recovery_test tests/dt/vesdemo_crash_recovery_test.cpp)
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
