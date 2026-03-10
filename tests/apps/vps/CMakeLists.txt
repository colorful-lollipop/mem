function(vps_add_test target source)
  add_executable(${target} ${source} ${PROJECT_SOURCE_DIR}/src/apps/vps/common/vps_codec.cpp)
  target_link_libraries(${target} PRIVATE memrpc GTest::gtest_main)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

vps_add_test(memrpc_vps_codec_test vps_codec_test.cpp)
