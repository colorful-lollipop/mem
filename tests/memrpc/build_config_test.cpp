#include <gtest/gtest.h>

#include <fstream>
#include <string>

TEST(BuildConfigTest, ExportCompileCommandsIsEnabled) {
  std::ifstream stream("/root/code/demo/mem/CMakeLists.txt");
  ASSERT_TRUE(stream.is_open());

  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("CMAKE_EXPORT_COMPILE_COMMANDS ON"), std::string::npos);
}

TEST(BuildConfigTest, CMakeExplicitlyRequiresCpp17) {
  std::ifstream stream("/root/code/demo/mem/CMakeLists.txt");
  ASSERT_TRUE(stream.is_open());

  std::string content((std::istreambuf_iterator<char>(stream)),
                      std::istreambuf_iterator<char>());
  EXPECT_NE(content.find("CMAKE_CXX_STANDARD 17"), std::string::npos);
  EXPECT_NE(content.find("CMAKE_CXX_STANDARD_REQUIRED ON"), std::string::npos);
}

TEST(BuildConfigTest, ParentRepoDoesNotDependOnOhosSaMock) {
  const char* kFiles[] = {
      "/root/code/demo/mem/CMakeLists.txt",
      "/root/code/demo/mem/src/CMakeLists.txt",
      "/root/code/demo/mem/BUILD.gn",
  };

  for (const char* path : kFiles) {
    std::ifstream stream(path);
    ASSERT_TRUE(stream.is_open()) << path;

    std::string content((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
    EXPECT_EQ(content.find("ohos_sa_mock"), std::string::npos) << path;
    EXPECT_EQ(content.find("third_party/ohos_sa_mock"), std::string::npos) << path;
  }
}

TEST(BuildConfigTest, SourceBuildOnlyKeepsCoreMemrpcAndMiniRpc) {
  struct FileExpectation {
    const char* path;
    const char* forbidden[8];
  };

  const FileExpectation expectations[] = {
      {"/root/code/demo/mem/src/CMakeLists.txt",
       {"client/engine_client.cpp",
        "server/engine_server.cpp",
        "memrpc/compat/scan_codec.cpp",
        "memrpc/compat/scan_behavior_codec.cpp",
        "src/rpc/",
        "vps_demo",
        "apps/vps/",
        nullptr}},
      {"/root/code/demo/mem/tests/CMakeLists.txt",
       {"memrpc_integration_end_to_end_test", "memrpc_notify_channel_test",
        "memrpc_rpc_notify_integration_test", nullptr}},
      {"/root/code/demo/mem/demo/CMakeLists.txt",
       {"memrpc_demo_dual_process", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
  };

  for (const auto& expectation : expectations) {
    std::ifstream stream(expectation.path);
    ASSERT_TRUE(stream.is_open()) << expectation.path;

    std::string content((std::istreambuf_iterator<char>(stream)),
                        std::istreambuf_iterator<char>());
    for (const char* token : expectation.forbidden) {
      if (token == nullptr) {
        break;
      }
      EXPECT_EQ(content.find(token), std::string::npos) << expectation.path << " still references "
                                                         << token;
    }
  }
}

TEST(BuildConfigTest, TestsAreSplitBetweenFrameworkAndMiniRpcDirectories) {
  std::ifstream root_stream("/root/code/demo/mem/tests/CMakeLists.txt");
  ASSERT_TRUE(root_stream.is_open());

  std::string root_content((std::istreambuf_iterator<char>(root_stream)),
                           std::istreambuf_iterator<char>());
  EXPECT_NE(root_content.find("add_subdirectory(memrpc)"), std::string::npos);
  EXPECT_NE(root_content.find("add_subdirectory(apps/minirpc)"), std::string::npos);
  EXPECT_NE(root_content.find("add_subdirectory(apps/vps)"), std::string::npos);

  std::ifstream memrpc_stream("/root/code/demo/mem/tests/memrpc/CMakeLists.txt");
  ASSERT_TRUE(memrpc_stream.is_open());

  std::string memrpc_content((std::istreambuf_iterator<char>(memrpc_stream)),
                             std::istreambuf_iterator<char>());
  EXPECT_NE(memrpc_content.find("memrpc_smoke_test"), std::string::npos);
  EXPECT_NE(memrpc_content.find("memrpc_session_test"), std::string::npos);

  std::ifstream minirpc_stream("/root/code/demo/mem/tests/apps/minirpc/CMakeLists.txt");
  ASSERT_TRUE(minirpc_stream.is_open());

  std::string minirpc_content((std::istreambuf_iterator<char>(minirpc_stream)),
                              std::istreambuf_iterator<char>());
  EXPECT_NE(minirpc_content.find("memrpc_minirpc_client_test"), std::string::npos);
  EXPECT_NE(minirpc_content.find("memrpc_minirpc_service_test"), std::string::npos);

  std::ifstream vps_stream("/root/code/demo/mem/tests/apps/vps/CMakeLists.txt");
  ASSERT_TRUE(vps_stream.is_open());

  std::string vps_content((std::istreambuf_iterator<char>(vps_stream)),
                          std::istreambuf_iterator<char>());
  EXPECT_NE(vps_content.find("memrpc_vps_codec_test"), std::string::npos);
}

TEST(BuildConfigTest, TopLevelMemrpcForwardingHeadersAreNotPartOfMainline) {
  const char* kLegacyHeaders[] = {
      "/root/code/demo/mem/include/memrpc/bootstrap.h",
      "/root/code/demo/mem/include/memrpc/types.h",
      "/root/code/demo/mem/include/memrpc/client.h",
      "/root/code/demo/mem/include/memrpc/demo_bootstrap.h",
      "/root/code/demo/mem/include/memrpc/sa_bootstrap.h",
      "/root/code/demo/mem/include/memrpc/handler.h",
      "/root/code/demo/mem/include/memrpc/rpc_client.h",
      "/root/code/demo/mem/include/memrpc/rpc_server.h",
      "/root/code/demo/mem/include/memrpc/server.h",
      nullptr,
  };

  for (const char** path = kLegacyHeaders; *path != nullptr; ++path) {
    std::ifstream stream(*path);
    EXPECT_FALSE(stream.is_open()) << *path << " should be removed from mainline";
  }
}
