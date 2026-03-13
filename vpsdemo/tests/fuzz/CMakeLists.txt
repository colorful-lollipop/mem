if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "VPSDEMO_ENABLE_FUZZ requires Clang")
endif()

add_executable(vpsdemo_codec_fuzz vesdemo_codec_fuzz.cpp)
set_target_properties(vpsdemo_codec_fuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)

target_link_libraries(vpsdemo_codec_fuzz PRIVATE vpsdemo_lib)

target_compile_options(vpsdemo_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

target_link_options(vpsdemo_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_test(NAME vpsdemo_codec_fuzz_smoke COMMAND vpsdemo_codec_fuzz -runs=1)
set_tests_properties(vpsdemo_codec_fuzz_smoke PROPERTIES
  LABELS "fuzz"
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0"
)

add_executable(vpsdemo_testkit_codec_fuzz testkit_codec_fuzz.cpp)
set_target_properties(vpsdemo_testkit_codec_fuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)

target_link_libraries(vpsdemo_testkit_codec_fuzz PRIVATE vpsdemo_lib)

target_compile_options(vpsdemo_testkit_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

target_link_options(vpsdemo_testkit_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_test(NAME vpsdemo_testkit_codec_fuzz_smoke COMMAND vpsdemo_testkit_codec_fuzz -runs=1)
set_tests_properties(vpsdemo_testkit_codec_fuzz_smoke PROPERTIES
  LABELS "fuzz"
  ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0;LSAN_OPTIONS=detect_leaks=0"
)
