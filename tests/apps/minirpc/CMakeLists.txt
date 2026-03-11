function(minirpc_add_test target source)
  add_executable(${target} ${source} ${ARGN})
  target_link_libraries(${target} PRIVATE minirpc_demo GTest::gtest_main)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

minirpc_add_test(memrpc_minirpc_headers_test minirpc_headers_test.cpp)
minirpc_add_test(memrpc_minirpc_codec_test minirpc_codec_test.cpp)
minirpc_add_test(memrpc_minirpc_service_test minirpc_service_test.cpp)
minirpc_add_test(memrpc_minirpc_client_test minirpc_client_test.cpp)
minirpc_add_test(memrpc_minirpc_throughput_test minirpc_throughput_test.cpp)
minirpc_add_test(memrpc_minirpc_baseline_test minirpc_baseline_test.cpp)
minirpc_add_test(memrpc_minirpc_latency_test minirpc_latency_test.cpp)
minirpc_add_test(memrpc_minirpc_async_pipeline_test minirpc_async_pipeline_test.cpp)
minirpc_add_test(memrpc_minirpc_backpressure_test minirpc_backpressure_test.cpp)
minirpc_add_test(memrpc_minirpc_dfx_test
  minirpc_dfx_test.cpp
  minirpc_resilient_invoker.cpp
)

if (MEMRPC_ENABLE_DT_TESTS)
  minirpc_add_test(memrpc_minirpc_dt_stability_test minirpc_dt_stability_test.cpp)
  set_tests_properties(memrpc_minirpc_dt_stability_test PROPERTIES LABELS "dt_stability")
endif()

if (MEMRPC_ENABLE_STRESS_TESTS)
  add_library(minirpc_stress_config
    minirpc_stress_config.cpp
  )
  target_include_directories(minirpc_stress_config PUBLIC ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(minirpc_stress_config PUBLIC minirpc_demo)
  target_compile_features(minirpc_stress_config PUBLIC cxx_std_17)

  add_executable(memrpc_minirpc_stress_config_test minirpc_stress_config_test.cpp)
  target_link_libraries(memrpc_minirpc_stress_config_test PRIVATE minirpc_stress_config GTest::gtest_main)
  add_test(NAME memrpc_minirpc_stress_config_test COMMAND memrpc_minirpc_stress_config_test)
  set_tests_properties(memrpc_minirpc_stress_config_test PROPERTIES LABELS "stress")

  add_executable(memrpc_minirpc_stress_runner minirpc_stress_runner.cpp)
  target_link_libraries(memrpc_minirpc_stress_runner PRIVATE minirpc_stress_config minirpc_demo)
  target_compile_features(memrpc_minirpc_stress_runner PRIVATE cxx_std_17)

  add_test(NAME memrpc_minirpc_stress_smoke COMMAND memrpc_minirpc_stress_runner)
  set_tests_properties(memrpc_minirpc_stress_smoke PROPERTIES
    LABELS "stress"
    ENVIRONMENT
      "MEMRPC_STRESS_DURATION_SEC=1;MEMRPC_STRESS_WARMUP_SEC=0;MEMRPC_STRESS_THREADS=1;MEMRPC_STRESS_ECHO_WEIGHT=100;MEMRPC_STRESS_ADD_WEIGHT=0;MEMRPC_STRESS_SLEEP_WEIGHT=0;MEMRPC_STRESS_PAYLOAD_SIZES=0,16;MEMRPC_STRESS_MAX_SLEEP_MS=1;MEMRPC_STRESS_SEED=1"
  )
endif()
