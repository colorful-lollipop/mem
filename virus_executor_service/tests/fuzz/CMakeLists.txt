if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "VIRUS_EXECUTOR_SERVICE_ENABLE_FUZZ requires Clang")
endif()

add_executable(virus_executor_service_codec_fuzz ves_codec_fuzz.cpp)
set_target_properties(virus_executor_service_codec_fuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)
target_link_libraries(virus_executor_service_codec_fuzz PRIVATE virus_executor_service_lib)
target_compile_options(virus_executor_service_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(virus_executor_service_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_test(NAME virus_executor_service_codec_fuzz_smoke
  COMMAND virus_executor_service_codec_fuzz -runs=1
)
set_tests_properties(virus_executor_service_codec_fuzz_smoke PROPERTIES
  LABELS "fuzz"
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0"
)

add_executable(virus_executor_service_testkit_codec_fuzz testkit_codec_fuzz.cpp)
set_target_properties(virus_executor_service_testkit_codec_fuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)
target_link_libraries(virus_executor_service_testkit_codec_fuzz PRIVATE virus_executor_service_lib)
target_compile_options(virus_executor_service_testkit_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
target_link_options(virus_executor_service_testkit_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_test(NAME virus_executor_service_testkit_codec_fuzz_smoke
  COMMAND virus_executor_service_testkit_codec_fuzz -runs=1
)
set_tests_properties(virus_executor_service_testkit_codec_fuzz_smoke PROPERTIES
  LABELS "fuzz"
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0"
)
