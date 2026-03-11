if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "MEMRPC_ENABLE_FUZZ requires Clang")
endif()

add_executable(minirpc_codec_fuzz minirpc_codec_fuzz.cpp)
set_target_properties(minirpc_codec_fuzz PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED YES
)

target_link_libraries(minirpc_codec_fuzz PRIVATE minirpc_demo)

target_compile_options(minirpc_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

target_link_options(minirpc_codec_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

add_test(NAME memrpc_minirpc_codec_fuzz_smoke COMMAND minirpc_codec_fuzz -runs=1)
set_tests_properties(memrpc_minirpc_codec_fuzz_smoke PROPERTIES LABELS "fuzz")
